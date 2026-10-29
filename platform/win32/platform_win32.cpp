#include "win32_platform.hpp"

#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace fitapp::win32 {

namespace {

fs::path exe_path() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return n ? fs::path(std::wstring(buf, n)) : fs::path();
}

// %LOCALAPPDATA%\FitSuite, or a "FitSuiteData" folder next to FitSuite.exe when the player made one
// (portable use, e.g. from a USB stick).
fs::path choose_data_dir() {
    std::error_code ec;
    const fs::path portable = exe_path().parent_path() / "FitSuiteData";
    if (fs::is_directory(portable, ec)) return portable;
    PWSTR base = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &base)) && base) {
        dir = fs::path(base) / "FitSuite";
        CoTaskMemFree(base);
    } else {
        dir = exe_path().parent_path() / "FitSuiteData";
    }
    fs::create_directories(dir, ec);
    return dir;
}

std::wstring make_filter(const std::string& desc, const std::string& pattern) {
    std::wstring f = widen(desc) + L" (" + widen(pattern) + L")";
    f.push_back(L'\0');
    f += widen(pattern);
    f.push_back(L'\0');
    f += L"All files (*.*)";
    f.push_back(L'\0');
    f += L"*.*";
    f.push_back(L'\0');
    f.push_back(L'\0');
    return f;
}

}  // namespace

Platform::Platform(HWND hwnd)
    : hwnd_(hwnd), data_dir_(choose_data_dir()), os_name_(os_display_name()), mouse_(hwnd), presentmon_(data_dir_) {}

Platform::~Platform() {
    stop_downloads();
    for (auto& [token, h] : programs_) CloseHandle(h);  // they keep running; we just stop watching
    set_cursor_captured(false);
    for (const auto& dev : temporary_changed_) {
        std::string err;
        restore_registry_mode(dev, &err);
    }
}

fit::PcInfo Platform::detect_pc() { return query_pc_info(); }

std::vector<DisplayInfo> Platform::displays() { return enumerate_displays(hwnd_, ddc_cache_); }

bool Platform::set_refresh_temporary(const std::string& device, double hz, std::string* error) {
    if (!change_refresh(device, hz, false, error)) return false;
    if (std::find(temporary_changed_.begin(), temporary_changed_.end(), device) == temporary_changed_.end())
        temporary_changed_.push_back(device);
    return true;
}

bool Platform::restore_display(const std::string& device, std::string* error) {
    auto it = std::find(temporary_changed_.begin(), temporary_changed_.end(), device);
    if (it == temporary_changed_.end()) return true;  // nothing was changed
    temporary_changed_.erase(it);
    return restore_registry_mode(device, error);
}

bool Platform::set_refresh_persistent(const std::string& device, double hz, std::string* error) {
    if (!change_refresh(device, hz, true, error)) return false;
    // The saved mode is now the new one, so there is nothing temporary left to undo.
    temporary_changed_.erase(std::remove(temporary_changed_.begin(), temporary_changed_.end(), device), temporary_changed_.end());
    return true;
}

bool Platform::window_focused() const { return focused_.load() && GetForegroundWindow() == hwnd_; }

void Platform::reapply_cursor_clip() {
    if (!cursor_captured_) {
        ClipCursor(nullptr);
        return;
    }
    RECT rc;
    GetClientRect(hwnd_, &rc);
    POINT c{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
    ClientToScreen(hwnd_, &c);
    // A tiny box in the middle of the window: the pointer cannot leave it or click anything else.
    RECT clip{c.x - 1, c.y - 1, c.x + 1, c.y + 1};
    SetCursorPos(c.x, c.y);
    ClipCursor(&clip);
}

void Platform::release_capture_now() {
    if (!cursor_captured_) return;
    cursor_captured_ = false;
    ClipCursor(nullptr);
}

void Platform::set_cursor_captured(bool captured) {
    if (captured && !window_focused()) captured = false;  // never capture a window in the background
    cursor_captured_ = captured;
    reapply_cursor_clip();
    if (!captured) {
        // Put the pointer back to a normal arrow right away instead of waiting for the next move.
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    } else {
        SetCursor(nullptr);
    }
}

void Platform::set_fullscreen(bool fullscreen) {
    if (fullscreen == fullscreen_) return;
    if (fullscreen) {
        saved_placement_.length = sizeof saved_placement_;
        GetWindowPlacement(hwnd_, &saved_placement_);
        saved_style_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, (saved_style_ & ~LONG_PTR(WS_OVERLAPPEDWINDOW)) | WS_POPUP);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, saved_style_);
        SetWindowPlacement(hwnd_, &saved_placement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    fullscreen_ = fullscreen;
    reapply_cursor_clip();
}

void Platform::set_render_mode(RenderMode mode, double limiter_fps) {
    mode_ = mode;
    limiter_fps_ = mode == RenderMode::MouseTest ? limiter_fps : 0.0;
}

std::optional<fs::path> Platform::open_file_dialog(const std::string& title, const std::string& filter_desc,
                                                   const std::string& pattern) {
    wchar_t file[MAX_PATH * 4] = {};
    const std::wstring filter = make_filter(filter_desc, pattern);
    const std::wstring wtitle = widen(title);
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = DWORD(std::size(file));
    ofn.lpstrTitle = wtitle.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return fs::path(file);
}

std::optional<fs::path> Platform::save_file_dialog(const std::string& title, const std::string& default_name,
                                                   const std::string& filter_desc, const std::string& pattern) {
    wchar_t file[MAX_PATH * 4] = {};
    const std::wstring def = widen(default_name);
    wcsncpy_s(file, def.c_str(), _TRUNCATE);
    const std::wstring filter = make_filter(filter_desc, pattern);
    const std::wstring wtitle = widen(title);
    std::wstring ext = widen(pattern);
    const size_t dot = ext.rfind(L'.');
    ext = dot == std::wstring::npos ? L"" : ext.substr(dot + 1);
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = DWORD(std::size(file));
    ofn.lpstrTitle = wtitle.c_str();
    ofn.lpstrDefExt = ext.empty() ? nullptr : ext.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return std::nullopt;
    return fs::path(file);
}

void Platform::open_folder(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return;
    ShellExecuteW(hwnd_, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Platform::open_url(const std::string& url) {
    // Only web pages and Windows' own Settings pages ("ms-settings:…"), never local programs.
    if (url.rfind("https://", 0) != 0 && url.rfind("ms-settings:", 0) != 0) return;
    if (url.find_first_of("\"\r\n") != std::string::npos) return;
    ShellExecuteW(hwnd_, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string Platform::probe_ddcci(const std::string& device) {
    const std::string r = probe_ddcci_device(device);
    ddc_cache_[device] = r;
    return r;
}

uint64_t Platform::start_program(const fs::path& exe, const std::vector<fs::path>& args, std::string* error) {
    std::error_code ec;
    if (!fs::is_regular_file(exe, ec)) {
        if (error) *error = narrow(exe.filename().wstring()) + " was not found.";
        return 0;
    }
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    for (const auto& a : args) cmd += L" \"" + a.wstring() + L"\"";  // Windows paths cannot contain quotes
    const std::wstring app = exe.wstring(), dir = exe.parent_path().wstring();
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    // Normal priority on purpose: while FitSuite waits in the background at a lower priority, a child
    // would otherwise inherit it, and Raw Accel's switch could lag behind a busy game.
    if (!CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | NORMAL_PRIORITY_CLASS, nullptr,
                        dir.c_str(), &si, &pi)) {
        if (error) *error = narrow(exe.filename().wstring()) + " could not be started (error " + std::to_string(GetLastError()) + ").";
        return 0;
    }
    CloseHandle(pi.hThread);
    const uint64_t token = ++next_program_;
    programs_[token] = pi.hProcess;
    return token;
}

std::optional<int> Platform::program_exit_code(uint64_t token) {
    auto it = programs_.find(token);
    if (it == programs_.end()) return -1;
    if (WaitForSingleObject(it->second, 0) == WAIT_TIMEOUT) return std::nullopt;
    DWORD code = 1;
    GetExitCodeProcess(it->second, &code);
    CloseHandle(it->second);
    programs_.erase(it);
    return int(code);
}

namespace {

// Executable names by process id, from a process snapshot: no handle to any process is opened, so
// games protected by anti-cheat are never touched.
std::map<DWORD, std::string> process_names() {
    std::map<DWORD, std::string> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof pe};
    if (Process32FirstW(snap, &pe)) {
        do {
            std::string n = narrow(pe.szExeFile);
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            out[pe.th32ProcessID] = n;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"FitSuite";
// Where Task Manager → Startup apps records entries the user switched off (binary; first byte odd = off).
constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

}  // namespace

std::string Platform::foreground_app() {
    HWND fg = GetForegroundWindow();
    if (!fg) return "";
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid) return "";
    if (pid == GetCurrentProcessId()) return "fitsuite.exe";  // also FitSuite's dialogs, whatever the exe is called
    if (fg == last_fg_ && pid == last_fg_pid_ && !last_fg_exe_.empty()) return last_fg_exe_;
    // The snapshot is only taken when a different window comes to the front.
    const auto names = process_names();
    auto it = names.find(pid);
    last_fg_ = fg;
    last_fg_pid_ = pid;
    last_fg_exe_ = it != names.end() ? it->second : std::string();
    return last_fg_exe_;
}

std::vector<std::pair<std::string, std::string>> Platform::running_apps() {
    struct Ctx {
        std::vector<std::pair<DWORD, std::wstring>> windows;
        HWND self;
    } ctx{{}, hwnd_};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            if (h == c->self || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
            if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
            DWORD cloaked = 0;  // suspended or hidden Store-app frames
            if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return TRUE;
            wchar_t title[256];
            if (GetWindowTextW(h, title, 256) <= 0) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid) c->windows.push_back({pid, title});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    const auto names = process_names();
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [pid, title] : ctx.windows) {
        auto it = names.find(pid);
        if (it == names.end() || pid == GetCurrentProcessId() || it->second == "explorer.exe" ||
            it->second == "applicationframehost.exe" || it->second == "textinputhost.exe" ||
            it->second == "systemsettings.exe")
            continue;
        bool dup = false;
        for (const auto& o : out) dup = dup || o.first == it->second;
        if (!dup) out.push_back({it->second, narrow(title)});
    }
    return out;
}

bool Platform::autostart_enabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

bool Platform::autostart_blocked() {
    BYTE data[32];
    DWORD size = sizeof data;
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kRunValue, RRF_RT_REG_BINARY, nullptr, data, &size) != ERROR_SUCCESS ||
        size < 1)
        return false;  // no record: Windows runs the entry
    return (data[0] & 1) != 0;  // 02 (or 06) = on, 03 (or 07) = switched off
}

void Platform::unblock_autostart() {
    // Only on the player's explicit request. Removing Task Manager's record leaves the entry switched on.
    if (autostart_blocked()) RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kRunValue);
}

bool Platform::set_autostart(bool on, std::string* error) {
    if (!on) {
        const LSTATUS r = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) {
            if (error) *error = "Could not remove FitSuite from the startup list (error " + std::to_string(r) + ").";
            return false;
        }
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kRunValue);  // Task Manager's record of it, if any
        return true;
    }
    // Per-user startup entry (no administrator rights). "--autostart" lets FitSuite tell a sign-in
    // start from a double-click (Settings decides whether it then opens its window).
    const std::wstring cmd = L"\"" + exe_path().wstring() + L"\" --autostart";
    wchar_t current[2048];
    DWORD size = sizeof current;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, current, &size) == ERROR_SUCCESS &&
        cmd == current)
        return true;  // already there
    const LSTATUS r = RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, cmd.c_str(),
                                      DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    if (r != ERROR_SUCCESS) {
        if (error) *error = "Could not add FitSuite to the startup list (error " + std::to_string(r) + ").";
        return false;
    }
    return true;
}

std::optional<bool> Platform::system_dark_mode() {
    if (!theme_known_) {
        // 0 = dark. Missing on Windows versions without an app mode: treat as light.
        DWORD light = 1, size = sizeof light;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                         L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS)
            light = 1;
        system_dark_ = light == 0;
        theme_known_ = true;
    }
    return system_dark_;
}

void Platform::set_dark_title_bar(bool dark) {
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
    const BOOL on = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &on, sizeof on);
    // Matches the tab strip (theme kBg0). Windows 11 only; earlier versions ignore it.
    const COLORREF caption = dark ? RGB(0x2B, 0x2B, 0x2B) : RGB(0xF0, 0xF0, 0xF0);
    DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR, &caption, sizeof caption);
}

void Platform::set_background_mode(bool on) {
    if (on == background_mode_) return;
    background_mode_ = on;
    // Windows' "efficiency mode" (EcoQoS) and a lower priority while FitSuite only waits in the
    // background; normal again as soon as its window is shown.
    PROCESS_POWER_THROTTLING_STATE st{};
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = on ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st);
    SetPriorityClass(GetCurrentProcess(), on ? BELOW_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
    if (on) SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));  // hand unused memory back
}

bool Platform::rawaccel_driver_present() {
    // Read-only check for the Raw Accel filter driver's service entry. FitSuite never talks to the
    // driver itself; Raw Accel's own writer applies settings.
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\rawaccel", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    RegCloseKey(k);
    return true;
}

}  // namespace fitapp::win32
