// FitSuite for Windows: Win32 window + Direct3D 11 flip-model swap chain + Dear ImGui.
//
// Presentation depends on what is on screen (RenderMode):
//   Ui         vsync, one frame per refresh.
//   MouseTest  no vsync (tearing when the system allows it) and a precise frame limiter, so the
//              scene follows the mouse with the least delay this window can offer.
//   MotionTest vsync, exactly one new frame per refresh; refreshes that repeated an old frame are
//              counted from DXGI frame statistics so the test can discard disturbed trials.
#include "win32_platform.hpp"

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

#include "app.hpp"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "implot.h"
#include "theme.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef NIN_SELECT
#define NIN_SELECT (WM_USER + 0)
#endif
#ifndef NIN_KEYSELECT
#define NIN_KEYSELECT (NIN_SELECT | 0x1)
#endif
#ifndef NIN_BALLOONUSERCLICK
#define NIN_BALLOONUSERCLICK (WM_USER + 5)
#endif
#ifndef NIF_SHOWTIP
#define NIF_SHOWTIP 0x00000080
#endif
#ifndef NIIF_RESPECT_QUIET_TIME
#define NIIF_RESPECT_QUIET_TIME 0x00000080
#endif

namespace {

using fitapp::win32::qpc_now_us;

constexpr wchar_t kWindowClass[] = L"FitSuiteMainWindow";
constexpr UINT kMsgTray = WM_APP + 1;  // notification-area icon events
constexpr UINT kMsgShow = WM_APP + 2;  // a second FitSuite.exe asks this one to show its window
constexpr UINT kTrayId = 1;
constexpr UINT kCmdOpen = 1, kCmdQuit = 2;

std::string hex(HRESULT hr) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(hr));
    return b;
}

template <typename T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// ---- Direct3D ---------------------------------------------------------------------------------

struct Renderer {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain1* swap = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    HANDLE waitable = nullptr;
    UINT swap_flags = 0;
    bool tearing = false;
    bool warp = false;
    bool flip_model = true;

    bool create(HWND hwnd, std::string* error);
    void destroy();
    bool create_rtv();
    void resize(UINT w, UINT h);
};

bool Renderer::create(HWND hwnd, std::string* error) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4,
                                   D3D11_SDK_VERSION, &device, &got, &ctx);
    if (hr == E_INVALIDARG)  // runtimes without 11.1
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 3,
                               D3D11_SDK_VERSION, &device, &got, &ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 3,
                               D3D11_SDK_VERSION, &device, &got, &ctx);
        warp = SUCCEEDED(hr);
    }
    if (FAILED(hr)) {
        if (error) *error = "Direct3D 11 is not available (error " + hex(hr) + ").";
        return false;
    }
    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_dev));
    if (dxgi_dev) dxgi_dev->GetAdapter(&adapter);
    if (adapter) adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
    release(adapter);
    release(dxgi_dev);
    if (!factory) {
        if (error) *error = "DXGI 1.2 is not available.";
        return false;
    }
    IDXGIFactory5* f5 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&f5)))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow))) tearing = allow != FALSE;
        release(f5);
    }
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.Scaling = DXGI_SCALING_NONE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
    hr = factory->CreateSwapChainForHwnd(device, hwnd, &sd, nullptr, nullptr, &swap);
    if (FAILED(hr)) {  // older systems: flip sequential, then the legacy blit model
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        tearing = false;
        hr = factory->CreateSwapChainForHwnd(device, hwnd, &sd, nullptr, nullptr, &swap);
    }
    if (FAILED(hr)) {
        sd.BufferCount = 1;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.Flags = 0;
        flip_model = false;
        hr = factory->CreateSwapChainForHwnd(device, hwnd, &sd, nullptr, nullptr, &swap);
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);  // FitSuite never uses exclusive fullscreen
    release(factory);
    if (FAILED(hr)) {
        if (error) *error = "The swap chain could not be created (error " + hex(hr) + ").";
        return false;
    }
    swap_flags = sd.Flags;
    if (sd.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        IDXGISwapChain2* s2 = nullptr;
        if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&s2)))) {
            s2->SetMaximumFrameLatency(1);
            waitable = s2->GetFrameLatencyWaitableObject();
            release(s2);
        }
    }
    return create_rtv();
}

bool Renderer::create_rtv() {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return false;
    const HRESULT hr = device->CreateRenderTargetView(back, nullptr, &rtv);
    release(back);
    return SUCCEEDED(hr);
}

void Renderer::resize(UINT w, UINT h) {
    release(rtv);
    swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swap_flags);
    create_rtv();
}

void Renderer::destroy() {
    release(rtv);
    if (waitable) {
        CloseHandle(waitable);
        waitable = nullptr;
    }
    release(swap);
    release(ctx);
    release(device);
}

// ---- precise frame limiter -------------------------------------------------------------------

class Limiter {
public:
    Limiter() {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    ~Limiter() {
        if (timer_) CloseHandle(timer_);
    }
    void reset() { next_us_ = 0; }
    void wait(double fps) {
        if (fps <= 0.0) {
            next_us_ = 0;
            return;
        }
        const int64_t period = int64_t(1e6 / fps);
        const int64_t now = qpc_now_us();
        if (next_us_ == 0 || now - next_us_ > 2 * period) next_us_ = now;  // resynchronise after a stall
        next_us_ += period;
        const int64_t remaining = next_us_ - now;
        if (timer_ && remaining > 1500) {
            LARGE_INTEGER due;
            due.QuadPart = -(remaining - 1000) * 10;  // relative, 100 ns units; wake ~1 ms early
            if (SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer_, 100);
        }
        while (qpc_now_us() < next_us_) YieldProcessor();  // finish on the clock
    }

private:
    HANDLE timer_ = nullptr;
    int64_t next_us_ = 0;
};

// ---- window state shared with the window procedure --------------------------------------------

struct WindowState {
    fitapp::win32::Platform* platform = nullptr;
    fitapp::App* app = nullptr;
    UINT resize_w = 0, resize_h = 0;
    bool minimized = false;
    bool focus_lost = false;  // App::on_focus_lost runs between frames, never inside one
    bool refresh_dirty = true;
    float scale = 1.0f;       // UI scale from the monitor DPI
    // Notification area ("system tray"). While MouseFit's per-game fix is on, closing the window
    // only hides it; FitSuite keeps switching the fix and can be reopened or quit from its icon.
    bool hidden = false;
    bool hide_requested = false, show_requested = false, quit_requested = false;
    bool tray_added = false;
    UINT taskbar_created = 0;
    HICON tray_icon = nullptr;
    std::string tray_text;
    int64_t last_activity_us = 0;  // last input or window message: the UI redraws at full rate for 0.5 s after it
    bool foreground_event = false;  // another program came to the front (WinEvent hook)
    bool displays_changed = false;  // WM_DISPLAYCHANGE: re-read refresh rates
} g;

void CALLBACK on_foreground_event(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) { g.foreground_event = true; }

void tray_add(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kMsgTray;
    if (!g.tray_icon)
        g.tray_icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    nid.hIcon = g.tray_icon;
    lstrcpynW(nid.szTip, fitapp::win32::widen(g.tray_text.empty() ? "FitSuite" : g.tray_text).c_str(), ARRAYSIZE(nid.szTip));
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) return;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    g.tray_added = true;
}

void tray_remove(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g.tray_added = false;
}

void tray_tip(HWND hwnd, const std::string& text) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    lstrcpynW(nid.szTip, fitapp::win32::widen(text).c_str(), ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void tray_balloon(HWND hwnd, const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    lstrcpynW(nid.szInfoTitle, title, ARRAYSIZE(nid.szInfoTitle));
    lstrcpynW(nid.szInfo, text, ARRAYSIZE(nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void tray_menu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, fitapp::win32::widen(g.tray_text.empty() ? "FitSuite" : g.tray_text).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kCmdOpen, L"Open FitSuite");
    AppendMenuW(m, MF_STRING, kCmdQuit, L"Quit FitSuite");
    SetMenuDefaultItem(m, kCmdOpen, FALSE);
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);  // required for the menu to close when clicking elsewhere
    const UINT cmd = UINT(TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr));
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd == kCmdOpen) g.show_requested = true;
    if (cmd == kCmdQuit) g.quit_requested = true;
}

void on_deactivated() {
    if (!g.platform) return;
    g.platform->on_focus_change(false);
    g.platform->release_capture_now();
    g.focus_lost = true;
}

// Dear ImGui works in logical (96 DPI) units; Windows sends physical pixels. Convert the pointer
// position before the backend sees it. Rendering is scaled through DisplayFramebufferScale.
LRESULT imgui_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const float s = g.scale;
    if (s != 1.0f) {
        if (msg == WM_MOUSEMOVE) {
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            lp = MAKELPARAM(WORD(short(std::lround(x / s))), WORD(short(std::lround(y / s))));
        } else if (msg == WM_NCMOUSEMOVE) {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &p);
            POINT q{LONG(std::lround(p.x / s)), LONG(std::lround(p.y / s))};
            ClientToScreen(h, &q);
            lp = MAKELPARAM(WORD(short(q.x)), WORD(short(q.y)));
        }
    }
    return ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // While a test holds the pointer, keep it hidden.
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && g.platform && g.platform->cursor_captured()) {
        SetCursor(nullptr);
        return TRUE;
    }
    if (ImGui::GetCurrentContext() && imgui_wndproc(h, msg, wp, lp)) return TRUE;
    switch (msg) {
        case WM_SIZE:
            g.minimized = wp == SIZE_MINIMIZED;
            if (!g.minimized) {
                g.resize_w = LOWORD(lp);
                g.resize_h = HIWORD(lp);
            }
            if (g.platform) g.platform->reapply_cursor_clip();
            return 0;
        case WM_MOVE:
            if (g.platform) g.platform->reapply_cursor_clip();
            break;
        case WM_DISPLAYCHANGE:
            g.displays_changed = true;
            g.refresh_dirty = true;
            break;
        case WM_EXITSIZEMOVE:
            g.refresh_dirty = true;
            break;
        case WM_SETTINGCHANGE:  // e.g. Windows switched between light and dark app mode
            if (g.platform) g.platform->on_setting_change();
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) on_deactivated();
            else if (g.platform) g.platform->on_focus_change(true);
            break;
        case WM_ACTIVATEAPP:
            if (!wp) on_deactivated();
            break;
        case WM_KILLFOCUS:
            on_deactivated();
            break;
        case WM_SETFOCUS:
            if (g.platform) g.platform->on_focus_change(true);
            break;
        case WM_DPICHANGED: {
            g.scale = float(HIWORD(wp)) / 96.0f;
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            g.refresh_dirty = true;
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = LONG(1080 * g.scale);
            mmi->ptMinTrackSize.y = LONG(680 * g.scale);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;  // Alt alone must not open the window menu
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            if (g.platform && g.platform->keep_running()) {  // keep switching the fix; hide instead
                g.hide_requested = true;
                return 0;
            }
            break;
        case kMsgShow:
            g.show_requested = true;
            return 0;
        case fitapp::win32::kMsgQuitApp:
            g.quit_requested = true;
            return 0;
        case kMsgTray:
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case NIN_BALLOONUSERCLICK:
                case WM_LBUTTONDBLCLK: g.show_requested = true; break;
                case WM_CONTEXTMENU: tray_menu(h); break;
                default: break;
            }
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;  // never hold up a shutdown or sign-out
        case WM_ENDSESSION:
            // Windows is shutting down or signing out and may end FitSuite as soon as this returns:
            // put the player's own Raw Accel settings back (the fix is only for the games they picked)
            // and save. With Windows Fast Startup a shut-down PC resumes its drivers from hibernation, so
            // Raw Accel may still hold the last settings it was given.
            if (wp && g.app) g.app->shutdown();
            return 0;
        case WM_DESTROY:
            if (g.tray_added) tray_remove(h);
            PostQuitMessage(0);
            return 0;
        default:
            if (g.taskbar_created && msg == g.taskbar_created) g.tray_added = false;  // Explorer restarted: add the icon again
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}


void place_window(HWND hwnd) {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT wa = mi.rcWork;
    const int aw = wa.right - wa.left, ah = wa.bottom - wa.top;
    int w = int(1440 * g.scale), h = int(920 * g.scale);
    w = std::min(w, aw - 40);
    h = std::min(h, ah - 40);
    SetWindowPos(hwnd, nullptr, wa.left + (aw - w) / 2, wa.top + (ah - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void fatal(const std::string& text) {
    MessageBoxW(nullptr, fitapp::win32::widen(text).c_str(), L"FitSuite", MB_ICONERROR | MB_OK);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdline, int show) {
    // "--autostart": started by the Windows startup entry (Settings → Open FitSuite when Windows
    // starts). "--background" is the same entry from an earlier preview.
    const std::wstring args = cmdline ? std::wstring(cmdline) : std::wstring();
    const bool autostart = args.find(L"--autostart") != std::wstring::npos || args.find(L"--background") != std::wstring::npos;

    // "--updated": started by the previous version right after it replaced FitSuite.exe. It is still
    // closing, so wait for it instead of handing over to it.
    const bool updated = args.find(L"--updated") != std::wstring::npos;

    // One FitSuite at a time: two copies would write the same setup file.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\FitSuite.SingleInstance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS && updated) {
        const DWORD w = WaitForSingleObject(single, 20000);
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) return 0;
    } else if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kWindowClass, nullptr); other && !autostart) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, kMsgShow, 0, 0);  // it may be hidden in the notification area
        }
        return 0;
    }

    if (updated) {  // remove the previous version's file (it may take a moment to close)
        wchar_t self[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
        const std::wstring old = std::wstring(self, n) + L".old";
        for (int i = 0; i < 25 && GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(old.c_str()); ++i) Sleep(200);
    }

    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, kWindowClass, L"FitSuite", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        fatal("The FitSuite window could not be created.");
        return 1;
    }
    g.scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    if (g.scale <= 0.0f) g.scale = 1.0f;
    place_window(hwnd);

    // Graphics exist only while the window can be seen: started hidden with Windows, or closed to the
    // notification area, FitSuite holds no Direct3D device at all. The window is shown once the
    // settings are loaded (theme, start hidden or not).
    Renderer r;
    std::string err;
    g.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // the layout is fixed; nothing to remember
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplWin32_Init(hwnd);
    bool have_gpu = false;
    fitapp::theme::setup(1.0f);  // DPI is applied through DisplayFramebufferScale below
    auto gpu_up = [&]() -> bool {
        if (have_gpu) return true;
        r = Renderer();
        if (!r.create(hwnd, &err) || !ImGui_ImplDX11_Init(r.device, r.ctx)) {
            r.destroy();
            return false;
        }
        g.resize_w = g.resize_h = 0;  // the new swap chain already has the window's size
        have_gpu = true;
        return true;
    };
    auto gpu_down = [&]() {
        if (!have_gpu) return;
        ImGui_ImplDX11_Shutdown();  // the font texture is re-created by Dear ImGui after the next Init
        r.destroy();
        r = Renderer();
        have_gpu = false;
    };

    int exit_code = 0;
    {
        fitapp::win32::Platform platform(hwnd);
        g.platform = &platform;
        fitapp::App app(platform);
        g.app = &app;
        app.init();  // loads the settings and applies the theme (title bar included) before anything shows
        const fit::AppSettings st = app.setup().app;
        bool done = false;
        if (autostart && !st.start_with_windows) {  // an entry left from an older setting: tidy up and leave
            platform.set_autostart(false, nullptr);
            done = true;
        }
        g.hidden = !done && autostart && st.start_hidden;
        if (!done && !g.hidden) {
            if (!gpu_up()) {
                fatal("FitSuite needs Direct3D 11.\n\n" + err);
                done = true;
                exit_code = 1;
            } else {
                ShowWindow(hwnd, show == SW_SHOWMINIMIZED || autostart ? SW_SHOWNORMAL : show);
                UpdateWindow(hwnd);
            }
        }
        platform.set_background_mode(g.hidden);
        const std::filesystem::path hint_marker = platform.data_dir() / "notification-area-hint-shown";
        // While the per-game fix is on, another program coming to the front wakes the loop at once (no
        // polling needed). Out of context: Windows posts the event to this thread; nothing is loaded into
        // other programs. Without the per-game fix there is no hook, and program switches do not wake FitSuite.
        HWINEVENTHOOK fg_hook = nullptr;
        auto update_fg_hook = [&]() {
            const bool want = app.wants_foreground_events();
            if (want && !fg_hook)
                fg_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, on_foreground_event, 0, 0,
                                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            else if (!want && fg_hook) {
                UnhookWinEvent(fg_hook);
                fg_hook = nullptr;
            }
        };
        update_fg_hook();
        bool hires_timer = false;  // 1 ms timer resolution, only while a test runs

        Limiter limiter;
        fitapp::RenderMode last_mode = fitapp::RenderMode::Ui;
        int64_t last_frame_us = 0;
        uint64_t dropped = 0;
        DXGI_FRAME_STATISTICS prev_stats{};
        bool have_stats = false;
        int64_t prev_present_us = 0;
        double refresh_hz = 0.0;
        bool occluded = false;

        while (!done) {
            // Idle: the plain UI has nothing animating, so when nobody touches it FitSuite waits for input
            // (or 100 ms, for clocks and progress bars) instead of drawing every refresh. Tests always run
            // at full rate.
            const bool idle = platform.render_mode() == fitapp::RenderMode::Ui && !app.test_running() &&
                              qpc_now_us() - g.last_activity_us > 500'000;
            if (g.hidden) {  // nothing is drawn: sleep until a message, a program switch, or the app's next due work
                const double wait_s = std::clamp(app.seconds_until_next_tick(), 0.0, 60.0);
                MsgWaitForMultipleObjectsEx(0, nullptr, DWORD(wait_s * 1000.0), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
            else if (idle || g.minimized) {
                MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                if (r.waitable) WaitForSingleObjectEx(r.waitable, 0, TRUE);  // keep the frame-latency count balanced
            }
            else if (r.waitable && !occluded)
                WaitForSingleObjectEx(r.waitable, 100, TRUE);
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    done = true;
                    exit_code = int(msg.wParam);
                }
                if (msg.message != WM_TIMER && msg.message != WM_NULL) g.last_activity_us = qpc_now_us();
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (done) break;

            // Notification area: the icon is there while FitSuite keeps running or is hidden.
            const bool want_tray = platform.keep_running() || g.hidden;
            if (want_tray && !g.tray_added) tray_add(hwnd);
            if (!want_tray && g.tray_added) tray_remove(hwnd);
            if (g.tray_added) {
                const std::string t = app.tray_text();
                if (t != g.tray_text) {
                    g.tray_text = t;
                    tray_tip(hwnd, t);
                }
            }
            update_fg_hook();
            if (g.displays_changed) {
                g.displays_changed = false;
                app.refresh_displays();
            }
            if (g.foreground_event) {
                g.foreground_event = false;
                app.on_foreground_changed();
            }
            if (g.quit_requested) {
                g.quit_requested = false;
                DestroyWindow(hwnd);  // WM_DESTROY posts WM_QUIT
                continue;
            }
            if (g.hide_requested) {
                g.hide_requested = false;
                app.on_hidden();
                ShowWindow(hwnd, SW_HIDE);
                g.hidden = true;
                gpu_down();
                platform.set_background_mode(true);
                std::error_code ec;
                if (g.tray_added && !std::filesystem::exists(hint_marker, ec)) {
                    tray_balloon(hwnd, L"FitSuite is still running",
                                 L"It switches your MouseFit fix on in the games you picked. Right-click this icon to quit.");
                    std::ofstream(hint_marker) << "1";
                }
            }
            if (g.show_requested) {
                g.show_requested = false;
                if (g.hidden) {
                    platform.set_background_mode(false);
                    if (!gpu_up()) {
                        fatal("FitSuite needs Direct3D 11.\n\n" + err);
                        done = true;
                        exit_code = 1;
                        break;
                    }
                    g.hidden = false;
                    ShowWindow(hwnd, SW_SHOW);
                    g.last_activity_us = qpc_now_us();
                } else if (IsIconic(hwnd)) {
                    ShowWindow(hwnd, SW_RESTORE);
                }
                SetForegroundWindow(hwnd);
            }
            if (g.hidden) {
                app.background_tick();
                const std::string reminder = app.take_reminder();  // at most once a day
                if (!reminder.empty() && g.tray_added)
                    tray_balloon(hwnd, L"Time for a quick check", fitapp::win32::widen(reminder).c_str());
                const std::string update = app.take_update_notice();  // once per new version
                if (!update.empty() && g.tray_added)
                    tray_balloon(hwnd, L"FitSuite update available", fitapp::win32::widen(update).c_str());
                continue;
            }

            if (g.focus_lost) {
                g.focus_lost = false;
                app.on_focus_lost();
            }
            if (g.resize_w && g.resize_h) {
                r.resize(g.resize_w, g.resize_h);
                g.resize_w = g.resize_h = 0;
            }
            if (g.refresh_dirty) {
                refresh_hz = fitapp::win32::window_refresh_hz(hwnd);
                g.refresh_dirty = false;
            }
            const fitapp::RenderMode mode = platform.render_mode();
            if (mode != last_mode) {
                last_mode = mode;
                have_stats = false;
                prev_present_us = 0;
                limiter.reset();
                // Fine timer resolution costs power system-wide; only the tests need it.
                const bool want_hires = mode != fitapp::RenderMode::Ui;
                if (want_hires != hires_timer) {
                    if (want_hires) timeBeginPeriod(1);
                    else timeEndPeriod(1);
                    hires_timer = want_hires;
                }
            }

            const int64_t now = qpc_now_us();
            fitapp::FrameTiming t;
            t.last_interval_ms = last_frame_us ? double(now - last_frame_us) / 1000.0 : 0.0;
            last_frame_us = now;
            t.refresh_hz = refresh_hz;
            t.tearing_supported = r.tearing;
            t.dropped_frames = dropped;
            t.limiter_fps = platform.limiter_fps();
            platform.update_timing(t);

            // During a mouse test the pointer belongs to the test: hidden, and ignored by the UI.
            if (platform.cursor_captured())
                io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
            else
                io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange);

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            const float s = g.scale;
            io.DisplaySize = ImVec2(io.DisplaySize.x / s, io.DisplaySize.y / s);
            io.DisplayFramebufferScale = ImVec2(s, s);
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();

            if (g.minimized) continue;  // keep the app logic (timers, auto-revert) running, skip drawing
            const ImVec4 bg = fitapp::theme::v4(fitapp::theme::kBg1);
            const float clear[4] = {bg.x, bg.y, bg.z, 1.0f};
            r.ctx->OMSetRenderTargets(1, &r.rtv, nullptr);
            r.ctx->ClearRenderTargetView(r.rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            HRESULT hr;
            if (mode == fitapp::RenderMode::MouseTest)
                hr = r.swap->Present(0, r.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u);
            else if (mode == fitapp::RenderMode::MotionTest)
                hr = r.swap->Present(1, 0);  // exactly one new frame per refresh
            else  // the UI needs no more than about 60 frames per second, even on a 240 Hz monitor
                hr = r.swap->Present(refresh_hz > 90.0 ? std::min(4u, UINT(std::lround(refresh_hz / 60.0))) : 1u, 0);
            occluded = hr == DXGI_STATUS_OCCLUDED;
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
                // Driver update or GPU reset: rebuild the device and carry on.
                app.on_focus_lost();
                ImGui_ImplDX11_Shutdown();
                r.destroy();
                r = Renderer();
                if (!r.create(hwnd, &err) || !ImGui_ImplDX11_Init(r.device, r.ctx)) {
                    have_gpu = false;  // nothing left to shut down
                    fatal("The graphics device stopped responding and could not be restarted.\n\n" + err);
                    done = true;
                    exit_code = 2;
                }
                continue;
            }

            if (mode == fitapp::RenderMode::MotionTest) {
                // A refresh that showed an old frame again means a frame missed its slot.
                DXGI_FRAME_STATISTICS st{};
                if (SUCCEEDED(r.swap->GetFrameStatistics(&st))) {
                    if (have_stats) {
                        const UINT dp = st.PresentCount - prev_stats.PresentCount;
                        const UINT dr = st.PresentRefreshCount - prev_stats.PresentRefreshCount;
                        if (dp > 0 && dr > dp && dr - dp < 120) dropped += dr - dp;
                    }
                    prev_stats = st;
                    have_stats = true;
                } else {
                    // Statistics unavailable: fall back to the time between presents.
                    have_stats = false;
                    const int64_t pnow = qpc_now_us();
                    if (prev_present_us && refresh_hz > 0) {
                        const double period_us = 1e6 / refresh_hz;
                        const double gap = double(pnow - prev_present_us);
                        if (gap > 1.5 * period_us) dropped += uint64_t(std::lround(gap / period_us)) - 1;
                    }
                    prev_present_us = pnow;
                }
            }
            if (mode == fitapp::RenderMode::MouseTest) limiter.wait(platform.limiter_fps());
            if (occluded && mode == fitapp::RenderMode::Ui) Sleep(16);
            // Behind another window (a game, say): redraw a few times per second only, so FitSuite
            // costs next to nothing while you play.
            else if (mode == fitapp::RenderMode::Ui && !platform.window_focused()) Sleep(66);
        }
        app.shutdown();
        if (fg_hook) UnhookWinEvent(fg_hook);
        if (hires_timer) timeEndPeriod(1);
        if (g.tray_added && IsWindow(hwnd)) tray_remove(hwnd);
        g.app = nullptr;
        g.platform = nullptr;
    }  // Platform destructor: releases the pointer and restores temporary display changes
    if (g.tray_icon) DestroyIcon(g.tray_icon);

    if (have_gpu) ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    r.destroy();
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    if (single) CloseHandle(single);
    return exit_code;
}
