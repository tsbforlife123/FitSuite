// FitSuite-Setup.exe: installs FitSuite for the current Windows account (no administrator rights).
//
// It carries FitSuite.exe as resource 2 and, when the player clicks Install:
//   1. asks a running FitSuite to quit,
//   2. writes FitSuite.exe into the chosen folder (%LOCALAPPDATA%\Programs\FitSuite by default),
//      replacing an earlier copy there,
//   3. adds Start menu and desktop shortcuts (each can be unticked),
//   4. adds FitSuite to Windows Settings → Apps, where Uninstall runs "FitSuite.exe --uninstall",
//   5. points an existing "Open FitSuite when Windows starts" entry at the installed copy,
//   6. opens FitSuite (can be unticked).
// FitSuite's data folder (%LOCALAPPDATA%\FitSuite) is not touched, so settings and measurements from
// an earlier copy carry over. FitSuite updates itself in place afterwards.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <string>

#include "install_paths.hpp"

namespace {

using namespace fitinstall;

enum : int { kIdChange = 101, kIdStartMenu, kIdDesktop, kIdOpen, kIdInstall, kIdCancel };

struct Ui {
    HWND wnd = nullptr, intro = nullptr, title = nullptr, label = nullptr, path = nullptr, change = nullptr;
    HWND start_menu = nullptr, desktop = nullptr, open = nullptr, status = nullptr, install = nullptr, cancel = nullptr;
    HFONT font = nullptr, title_font = nullptr;
    UINT dpi = 96;
    std::wstring dir;
    bool done = false;
} g;

int px(int v) { return MulDiv(v, int(g.dpi), 96); }

std::wstring installed_dir() {  // where an earlier install put FitSuite, if any
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS &&
        buf[0])
        return buf;
    return {};
}

bool payload(const void** data, DWORD* size) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(2), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return false;
    HGLOBAL h = LoadResource(nullptr, r);
    *size = SizeofResource(nullptr, r);
    *data = h ? LockResource(h) : nullptr;
    return *data && *size > 1024;
}

bool write_file(const std::wstring& p, const void* d, DWORD n) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(f, d, n, &w, nullptr) && w == n;
    ok = FlushFileBuffers(f) && ok;
    CloseHandle(f);
    if (!ok) DeleteFileW(p.c_str());
    return ok;
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool make_shortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& dir) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&sl))))
        return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(dir.c_str());
    sl->SetDescription(L"FitSuite");
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    HRESULT hr = sl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf));
    if (SUCCEEDED(hr)) {
        hr = pf->Save(lnk.c_str(), TRUE);
        pf->Release();
    }
    sl->Release();
    return SUCCEEDED(hr);
}

void set_str(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t)));
}

void set_dword(HKEY k, const wchar_t* name, DWORD v) {
    RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v);
}

bool register_app(const std::wstring& exe, DWORD size_bytes) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    set_str(k, L"DisplayName", L"FitSuite");
    set_str(k, L"Publisher", L"FitSuite");
    set_str(k, L"DisplayIcon", exe);
    set_str(k, L"InstallLocation", g.dir);
    set_str(k, L"UninstallString", L"\"" + exe + L"\" --uninstall");
    set_str(k, L"URLInfoAbout", L"https://github.com/tsbforlife123/FitSuite");
    set_dword(k, L"EstimatedSize", size_bytes / 1024);
    set_dword(k, L"NoModify", 1);
    set_dword(k, L"NoRepair", 1);
    RegCloseKey(k);
    return true;
}

// "Open FitSuite when Windows starts" (set in FitSuite) starts the installed copy from now on.
void move_startup_entry(const std::wstring& exe) {
    wchar_t cur[2048];
    DWORD size = sizeof cur;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, cur, &size) != ERROR_SUCCESS) return;
    const std::wstring cmd = L"\"" + exe + L"\" --autostart";
    RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, cmd.c_str(), DWORD((cmd.size() + 1) * sizeof(wchar_t)));
}

void set_status(const wchar_t* t) {
    SetWindowTextW(g.status, t);
    UpdateWindow(g.wnd);
}

void fail(const std::wstring& m) {
    set_status(L"");
    MessageBoxW(g.wnd, m.c_str(), L"FitSuite setup", MB_ICONERROR | MB_OK);
    EnableWindow(g.install, TRUE);
    EnableWindow(g.change, TRUE);
}

void install() {
    EnableWindow(g.install, FALSE);
    EnableWindow(g.change, FALSE);
    const void* data = nullptr;
    DWORD size = 0;
    if (!payload(&data, &size)) return fail(L"This setup file is damaged. Download FitSuite-Setup.exe again.");

    set_status(L"Closing FitSuite...");
    if (!close_running_fitsuite(15000))
        return fail(L"FitSuite is still open. Quit it (right-click its icon next to the clock, then Quit FitSuite) and "
                    L"click Install again.");

    set_status(L"Installing...");
    const int r = SHCreateDirectoryExW(nullptr, g.dir.c_str(), nullptr);
    if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS && r != ERROR_FILE_EXISTS)
        return fail(L"Windows did not allow creating the folder\n\n" + g.dir +
                    L"\n\nChoose a folder in your own account, for example the one Setup suggests.");
    const std::wstring exe = g.dir + L"\\FitSuite.exe", tmp = exe + L".new", old = exe + L".old";
    if (!write_file(tmp, data, size))
        return fail(L"Windows did not allow writing to\n\n" + g.dir + L"\n\nChoose a folder in your own account.");
    // An earlier copy is renamed first (a file that is still closing cannot be overwritten, but can be
    // renamed), and put back if anything goes wrong.
    const bool had_old = exists(exe);
    if (had_old) {
        DeleteFileW(old.c_str());
        if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmp.c_str());
            return fail(L"The FitSuite already in this folder could not be replaced. Close it and try again.");
        }
    }
    if (!MoveFileExW(tmp.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (had_old) MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(tmp.c_str());
        return fail(L"FitSuite could not be put in place in\n\n" + g.dir);
    }
    for (int i = 0; i < 20 && exists(old) && !DeleteFileW(old.c_str()); ++i) Sleep(200);

    std::wstring problems;
    const std::wstring sm = start_menu_shortcut(), dt = desktop_shortcut();
    if (SendMessageW(g.start_menu, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        if (!make_shortcut(sm, exe, g.dir)) problems += L"\n- The Start menu shortcut could not be made.";
    } else if (exists(sm)) {
        DeleteFileW(sm.c_str());
    }
    if (SendMessageW(g.desktop, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        if (!make_shortcut(dt, exe, g.dir)) problems += L"\n- The desktop shortcut could not be made.";
    } else if (exists(dt)) {
        DeleteFileW(dt.c_str());
    }
    if (!register_app(exe, size)) problems += L"\n- FitSuite could not be added to Windows Settings > Apps.";
    move_startup_entry(exe);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (!problems.empty())
        MessageBoxW(g.wnd, (L"FitSuite is installed, with a problem:" + problems).c_str(), L"FitSuite setup",
                    MB_ICONWARNING | MB_OK);

    if (SendMessageW(g.open, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, g.dir.c_str(), SW_SHOWNORMAL);
        DestroyWindow(g.wnd);
        return;
    }
    g.done = true;
    set_status(L"FitSuite is installed.");
    ShowWindow(g.install, SW_HIDE);
    SetWindowTextW(g.cancel, L"Close");
}

void choose_folder() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, reinterpret_cast<void**>(&d))))
        return;
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Choose where to install FitSuite");
    if (SUCCEEDED(d->Show(g.wnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                std::wstring dir = p;
                CoTaskMemFree(p);
                while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
                // FitSuite gets a folder of its own inside the one chosen.
                const size_t slash = dir.find_last_of(L"\\/");
                const std::wstring last = slash == std::wstring::npos ? dir : dir.substr(slash + 1);
                if (lstrcmpiW(last.c_str(), L"FitSuite") != 0) dir += (dir.back() == L'\\' ? L"" : L"\\") + std::wstring(L"FitSuite");
                g.dir = dir;
                SetWindowTextW(g.path, g.dir.c_str());
            }
            it->Release();
        }
    }
    d->Release();
}

HWND control(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), g.wnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return c;
}

void make_fonts() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);  // at the system DPI
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW t = ncm.lfMessageFont;
    t.lfHeight = MulDiv(t.lfHeight, 3, 2);
    t.lfWeight = FW_SEMIBOLD;
    g.title_font = CreateFontIndirectW(&t);
}

void build() {
    make_fonts();
    g.title = control(L"STATIC", L"Install FitSuite", SS_LEFT, 24, 20, 472, 32);
    SendMessageW(g.title, WM_SETFONT, reinterpret_cast<WPARAM>(g.title_font), TRUE);
    g.intro = control(L"STATIC",
                      L"FitSuite is installed for your Windows account only, so it does not need administrator rights. "
                      L"Settings and measurements from an earlier copy are kept.",
                      SS_LEFT, 24, 60, 472, 40);
    g.label = control(L"STATIC", L"Install to:", SS_LEFT, 24, 110, 472, 20);
    g.path = control(L"EDIT", g.dir.c_str(), WS_BORDER | ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP, 24, 132, 372, 26);
    g.change = control(L"BUTTON", L"Change...", BS_PUSHBUTTON | WS_TABSTOP, 404, 131, 92, 28, kIdChange);
    g.start_menu = control(L"BUTTON", L"Add FitSuite to the Start menu", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 172, 472, 24, kIdStartMenu);
    g.desktop = control(L"BUTTON", L"Add a shortcut on the desktop", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 198, 472, 24, kIdDesktop);
    g.open = control(L"BUTTON", L"Open FitSuite when Setup is done", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 224, 472, 24, kIdOpen);
    for (HWND b : {g.start_menu, g.desktop, g.open}) SendMessageW(b, BM_SETCHECK, BST_CHECKED, 0);
    g.status = control(L"STATIC", L"", SS_LEFT, 24, 274, 280, 22);
    g.install = control(L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, 312, 268, 88, 32, kIdInstall);
    g.cancel = control(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 408, 268, 88, 32, kIdCancel);
    SetFocus(g.install);
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdChange: choose_folder(); return 0;
                case kIdInstall:
                case IDOK:  // Enter
                    if (!g.done) install();
                    return 0;
                case kIdCancel:
                case IDCANCEL: DestroyWindow(h); return 0;
                default: break;
            }
            break;
        case WM_CTLCOLORSTATIC: {  // labels and check boxes on the window's own background
            HDC dc = reinterpret_cast<HDC>(wp);
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g.dir = installed_dir();
    if (g.dir.empty()) g.dir = default_install_dir();
    if (HDC screen = GetDC(nullptr)) {  // the system DPI (Setup is DPI aware through its manifest)
        g.dpi = UINT(GetDeviceCaps(screen, LOGPIXELSY));
        ReleaseDC(nullptr, screen);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"FitSuiteSetupWindow";
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, px(520), px(324)};
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g.wnd = CreateWindowExW(0, wc.lpszClassName, L"FitSuite setup", style, wa.left + (wa.right - wa.left - w) / 2,
                            wa.top + (wa.bottom - wa.top - h) / 2, w, h, nullptr, nullptr, instance, nullptr);
    if (!g.wnd) return 1;
    build();
    ShowWindow(g.wnd, show);
    UpdateWindow(g.wnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g.wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g.font) DeleteObject(g.font);
    if (g.title_font) DeleteObject(g.title_font);
    CoUninitialize();
    return 0;
}
