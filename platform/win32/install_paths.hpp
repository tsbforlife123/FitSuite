// Where FitSuite-Setup.exe puts things, shared with FitSuite.exe's "--uninstall". Everything is for
// the current Windows account only (no administrator rights): the program in
// %LOCALAPPDATA%\Programs\FitSuite, shortcuts in the Start menu and on the desktop, and an entry in
// Windows Settings → Apps. FitSuite's data (%LOCALAPPDATA%\FitSuite) is separate and kept.
#pragma once

#include <windows.h>
#include <shlobj.h>

#include <string>

namespace fitinstall {

inline constexpr wchar_t kMainWindowClass[] = L"FitSuiteMainWindow";
inline constexpr wchar_t kSingleInstanceMutex[] = L"Local\\FitSuite.SingleInstance";
inline constexpr UINT kMsgQuitApp = WM_APP + 3;  // same as fitapp::win32::kMsgQuitApp
inline constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FitSuite";
inline constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t kRunValue[] = L"FitSuite";
inline constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

inline std::wstring known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p)) && p) out = p;
    if (p) CoTaskMemFree(p);
    return out;
}

inline std::wstring default_install_dir() {
    std::wstring base = known_folder(FOLDERID_UserProgramFiles);  // %LOCALAPPDATA%\Programs
    if (base.empty()) base = known_folder(FOLDERID_LocalAppData) + L"\\Programs";
    return base + L"\\FitSuite";
}

inline std::wstring start_menu_shortcut() { return known_folder(FOLDERID_Programs) + L"\\FitSuite.lnk"; }
inline std::wstring desktop_shortcut() { return known_folder(FOLDERID_Desktop) + L"\\FitSuite.lnk"; }

// Asks a running FitSuite to quit (it puts the player's own Raw Accel settings back on the way out)
// and waits up to `ms` for it. True when no FitSuite is running any more.
inline bool close_running_fitsuite(DWORD ms) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, kSingleInstanceMutex);
    if (!m) return true;  // not running
    if (HWND w = FindWindowW(kMainWindowClass, nullptr)) PostMessageW(w, kMsgQuitApp, 0, 0);
    const DWORD r = WaitForSingleObject(m, ms);
    const bool gone = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
    if (gone) ReleaseMutex(m);
    CloseHandle(m);
    if (gone) Sleep(300);  // the process may hold its file for a moment after letting go
    return gone;
}

}  // namespace fitinstall
