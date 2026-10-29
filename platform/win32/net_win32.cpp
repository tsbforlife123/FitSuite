// Downloads (WinHTTP), unpacking, elevated installers, Raw Accel discovery and self-update.
//
// Network use is limited to what the player asked for: the update check (when switched on in
// Settings or run with "Check now") and the setup's Raw Accel download. Every download goes over
// HTTPS to a file next to its final name and is only used after its SHA-256 has been checked by the
// caller.
#include "win32_platform.hpp"  // windows.h first

#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <fstream>

#include "fit/profile.hpp"

namespace fs = std::filesystem;

namespace fitapp::win32 {

struct Platform::Download {
    std::thread thread;
    std::atomic<bool> done{false}, ok{false}, cancel{false};
    std::atomic<uint64_t> bytes{0}, total{0};
    std::mutex m;
    std::string error;
};

namespace {

void set_error(Platform::Download& d, const std::string& e) {
    std::lock_guard<std::mutex> lock(d.m);
    if (d.error.empty()) d.error = e;
}

std::string win_error(const char* what) {
    return std::string(what) + " (error " + std::to_string(GetLastError()) + ")";
}

void run_download(std::shared_ptr<Platform::Download> d, std::wstring url, fs::path dest) {
    const fs::path part = dest.string() + ".part";
    HINTERNET session = nullptr, conn = nullptr, req = nullptr;
    auto cleanup = [&]() {
        if (req) WinHttpCloseHandle(req);
        if (conn) WinHttpCloseHandle(conn);
        if (session) WinHttpCloseHandle(session);
    };
    const std::wstring agent = L"FitSuite/" + widen(fit::kAppVersion);
    session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)  // Windows before 8.1 does not know the automatic proxy type
        session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        set_error(*d, win_error("Could not start the download"));
        d->done = true;
        return;
    }
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        set_error(*d, "Only https:// addresses are downloaded.");
        cleanup();
        d->done = true;
        return;
    }
    conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (conn) req = WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    DWORD status = 0, size = sizeof status;
    if (!req || !WinHttpSendRequest(req, L"Accept: */*\r\n", DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr) ||
        !WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        set_error(*d, win_error("Could not reach the server. Check your internet connection"));
        cleanup();
        d->done = true;
        return;
    }
    if (status != 200) {
        set_error(*d, "The server answered " + std::to_string(status) + ".");
        cleanup();
        d->done = true;
        return;
    }
    wchar_t lenbuf[32] = {};
    DWORD lensize = sizeof lenbuf;
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenbuf, &lensize, WINHTTP_NO_HEADER_INDEX))
        d->total = uint64_t(_wtoi64(lenbuf));
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) {
        set_error(*d, "Could not write " + narrow(part.wstring()) + ".");
        cleanup();
        d->done = true;
        return;
    }
    std::vector<char> buf(1 << 16);
    bool good = true;
    for (;;) {
        if (d->cancel) {
            set_error(*d, "Cancelled.");
            good = false;
            break;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), DWORD(buf.size()), &got)) {
            set_error(*d, win_error("The download was interrupted"));
            good = false;
            break;
        }
        if (got == 0) break;
        out.write(buf.data(), got);
        d->bytes += got;
        if (d->bytes > (uint64_t(256) << 20)) {  // nothing FitSuite downloads is this large
            set_error(*d, "The download is larger than expected and was stopped.");
            good = false;
            break;
        }
    }
    out.close();
    cleanup();
    if (good && !out.fail()) {
        fs::remove(dest, ec);
        fs::rename(part, dest, ec);
        if (ec) set_error(*d, "Could not save " + narrow(dest.wstring()) + ".");
        else d->ok = true;
    } else {
        fs::remove(part, ec);
    }
    d->done = true;
}

fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR base = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &base)) && base) {
        out = fs::path(base);
        CoTaskMemFree(base);
    }
    return out;
}

bool contains_rawaccel(const std::wstring& name) {
    std::wstring n = name;
    std::transform(n.begin(), n.end(), n.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return n.find(L"rawaccel") != std::wstring::npos || n.find(L"raw accel") != std::wstring::npos;
}

}  // namespace

uint64_t Platform::start_download(const std::string& url, const fs::path& dest, std::string* error) {
    if (url.rfind("https://", 0) != 0) {
        if (error) *error = "Only https:// addresses are downloaded.";
        return 0;
    }
    auto d = std::make_shared<Download>();
    try {
        d->thread = std::thread(run_download, d, widen(url), dest);
    } catch (...) {
        if (error) *error = "Could not start the download.";
        return 0;
    }
    const uint64_t token = ++next_download_;
    downloads_[token] = d;
    return token;
}

IPlatform::DownloadStatus Platform::download_status(uint64_t token) {
    DownloadStatus s;
    auto it = downloads_.find(token);
    if (it == downloads_.end()) {
        s.done = true;
        s.error = "Unknown download.";
        return s;
    }
    Download& d = *it->second;
    s.bytes = d.bytes;
    s.total = d.total;
    if (d.done) {
        if (d.thread.joinable()) d.thread.join();
        s.done = true;
        s.ok = d.ok;
        std::lock_guard<std::mutex> lock(d.m);
        s.error = d.error;
    }
    return s;
}

void Platform::cancel_download(uint64_t token) {
    auto it = downloads_.find(token);
    if (it != downloads_.end()) it->second->cancel = true;
}

void Platform::stop_downloads() {
    for (auto& [token, d] : downloads_) {
        d->cancel = true;
        if (d->thread.joinable()) d->thread.join();
    }
    downloads_.clear();
}

uint64_t Platform::start_unzip(const fs::path& zip, const fs::path& dest, std::string* error) {
    // Windows 10 and 11 include bsdtar, which reads .zip files.
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::error_code ec;
    fs::create_directories(dest, ec);
    return start_program(fs::path(sys) / L"tar.exe", {L"-xf", zip, L"-C", dest}, error);
}

uint64_t Platform::start_elevated(const fs::path& exe, std::string* error) {
    const std::wstring file = exe.wstring(), dir = exe.parent_path().wstring();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = hwnd_;
    sei.lpVerb = L"runas";  // Windows asks the player for administrator rights
    sei.lpFile = file.c_str();
    sei.lpDirectory = dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        const DWORD e = GetLastError();
        if (error)
            *error = e == ERROR_CANCELLED ? std::string("You chose No at Windows' permission prompt, so nothing was installed.")
                                          : narrow(exe.filename().wstring()) + " could not be started (error " + std::to_string(e) + ").";
        return 0;
    }
    const uint64_t token = ++next_program_;
    programs_[token] = sei.hProcess;
    return token;
}

std::vector<fs::path> Platform::find_rawaccel_folders() {
    std::vector<fs::path> out;
    std::error_code ec;
    auto consider = [&](const fs::path& dir) {
        if (!fs::is_regular_file(dir / "writer.exe", ec)) return;
        for (const auto& o : out)
            if (fs::equivalent(o, dir, ec)) return;
        out.push_back(dir);
    };
    // Where the setup puts it first, then folders people usually unpack downloads into.
    consider(programs_dir() / "RawAccel");
    std::vector<fs::path> roots = {known_folder(FOLDERID_Downloads), known_folder(FOLDERID_Desktop),
                                   known_folder(FOLDERID_Documents), known_folder(FOLDERID_ProgramFiles), fs::path(L"C:\\")};
    for (const auto& root : roots) {
        if (root.empty() || !fs::is_directory(root, ec)) continue;
        fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (int n = 0; !ec && it != end && n < 2000; it.increment(ec), ++n) {
            if (!it->is_directory(ec) || !contains_rawaccel(it->path().filename().wstring())) continue;
            consider(it->path());
            // The release zip unpacks into RawAccel\ inside whatever folder it was extracted to.
            fs::directory_iterator sub(it->path(), fs::directory_options::skip_permission_denied, ec), send;
            for (int m = 0; !ec && sub != send && m < 200; sub.increment(ec), ++m)
                if (sub->is_directory(ec)) consider(sub->path());
            ec.clear();
        }
        ec.clear();
    }
    return out;
}

fit::WindowsGameSettings Platform::windows_game_settings() {
    fit::WindowsGameSettings w;
    DWORD v = 0, size = sizeof v;
    // Game Mode: on unless switched off (Windows writes the value only once it was changed).
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled", RRF_RT_REG_DWORD, nullptr, &v,
                     &size) == ERROR_SUCCESS)
        w.game_mode = v != 0;
    else
        w.game_mode = true;
    // Hardware-accelerated GPU scheduling: 2 on, 1 off, missing when Windows decides.
    v = 0;
    size = sizeof v;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", RRF_RT_REG_DWORD,
                     nullptr, &v, &size) == ERROR_SUCCESS &&
        (v == 1 || v == 2))
        w.hags = v == 2;
    // The active power plan, by the GUIDs Windows uses for its own plans.
    wchar_t guid[64] = {};
    DWORD gsize = sizeof guid;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes", L"ActivePowerScheme",
                     RRF_RT_REG_SZ, nullptr, guid, &gsize) == ERROR_SUCCESS) {
        std::wstring g = guid;
        for (auto& ch : g) ch = wchar_t(towlower(ch));
        if (g == L"381b4222-f694-41f0-9685-ff5bb260df2e") w.power_plan = "balanced";
        else if (g == L"8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c") w.power_plan = "high_performance";
        else if (g == L"e9a42b02-d5df-448d-aa00-03f14749eb61") w.power_plan = "ultimate";
        else if (g == L"a1841308-3541-4fab-bc81-f71556f20b4a") w.power_plan = "power_saver";
        else if (!g.empty()) w.power_plan = "other";
    }
    // "Enhance pointer precision": the third value of SPI_GETMOUSE.
    int mouse[3] = {};
    if (SystemParametersInfoW(SPI_GETMOUSE, 0, mouse, 0)) w.pointer_precision = mouse[2] != 0;
    return w;
}

bool Platform::vc_runtime_installed() {
    DWORD installed = 0, size = sizeof installed;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64", L"Installed",
                     RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &installed, &size) == ERROR_SUCCESS)
        return installed == 1;
    return false;
}

fs::path Platform::programs_dir() {
    fs::path base = known_folder(FOLDERID_UserProgramFiles);  // %LOCALAPPDATA%\Programs
    if (base.empty()) base = known_folder(FOLDERID_LocalAppData) / "Programs";
    return base;
}

fs::path Platform::self_exe() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return n ? fs::path(std::wstring(buf, n)) : fs::path();
}

bool Platform::apply_update(const fs::path& new_exe, std::string* error) {
    // A running program cannot be overwritten, but it can be renamed: FitSuite.exe → FitSuite.exe.old,
    // the new file → FitSuite.exe, then the new version starts and deletes the old one. Any failure
    // puts the original back.
    const fs::path cur = self_exe();
    const fs::path old = cur.wstring() + L".old";
    std::error_code ec;
    if (cur.empty() || !fs::is_regular_file(new_exe, ec)) {
        if (error) *error = "The downloaded update is missing.";
        return false;
    }
    DeleteFileW(old.c_str());
    if (!MoveFileExW(cur.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (error)
            *error = "FitSuite cannot replace its own file in " + narrow(cur.parent_path().wstring()) +
                     " (error " + std::to_string(GetLastError()) + "). Move the FitSuite folder somewhere you can write to, "
                     "such as Documents, or download the new version yourself.";
        return false;
    }
    if (!MoveFileExW(new_exe.c_str(), cur.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        const DWORD e = GetLastError();
        MoveFileExW(old.c_str(), cur.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (error) *error = "The new version could not be put in place (error " + std::to_string(e) + "). Nothing was changed.";
        return false;
    }
    std::wstring cmd = L"\"" + cur.wstring() + L"\" --updated";
    // Started with Windows (hidden in the notification area): the new version starts the same way.
    if (wcsstr(GetCommandLineW(), L"--autostart")) cmd += L" --autostart";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    const std::wstring dir = cur.parent_path().wstring();
    if (!CreateProcessW(cur.c_str(), cmd.data(), nullptr, nullptr, FALSE, NORMAL_PRIORITY_CLASS, nullptr, dir.c_str(), &si, &pi)) {
        const DWORD e = GetLastError();
        MoveFileExW(cur.c_str(), new_exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        MoveFileExW(old.c_str(), cur.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (error) *error = "The new version could not be started (error " + std::to_string(e) + "). Nothing was changed.";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void Platform::request_quit() { PostMessageW(hwnd_, kMsgQuitApp, 0, 0); }

bool Platform::restart_pc(std::string* error) {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    return start_program(fs::path(sys) / L"shutdown.exe", {L"/r", L"/t", L"5"}, error) != 0;
}

}  // namespace fitapp::win32
