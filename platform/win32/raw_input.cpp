#include "win32_platform.hpp"

#include <hidsdi.h>

#include <algorithm>
#include <cwctype>

namespace fitapp::win32 {

namespace {

constexpr UINT kStopMessage = WM_APP + 1;

std::string fnv_hex(const std::wstring& s) {
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : s) {
        h ^= uint64_t(std::towlower(c));
        h *= 1099511628211ull;
    }
    char buf[24];
    std::snprintf(buf, sizeof buf, "mouse-%016llx", (unsigned long long)h);
    return buf;
}

std::wstring device_path(HANDLE h) {
    UINT size = 0;
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, nullptr, &size) != 0 || size == 0) return L"";
    std::wstring s(size, L'\0');
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, s.data(), &size) == UINT(-1)) return L"";
    while (!s.empty() && s.back() == L'\0') s.pop_back();
    return s;
}

std::string vid_pid_from_path(const std::wstring& p) {
    std::wstring up = p;
    std::transform(up.begin(), up.end(), up.begin(), ::towupper);
    const size_t v = up.find(L"VID_"), d = up.find(L"PID_");
    if (v == std::wstring::npos || d == std::wstring::npos || v + 8 > up.size() || d + 8 > up.size()) return "";
    return narrow(up.substr(v + 4, 4)) + ":" + narrow(up.substr(d + 4, 4));
}

std::string product_string(const std::wstring& path) {
    // Opening with no access rights is enough to read HID strings, even for mice Windows owns.
    HANDLE f = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    wchar_t buf[256] = {};
    std::string out;
    if (HidD_GetProductString(f, buf, sizeof buf)) out = narrow(buf);
    CloseHandle(f);
    return out;
}

}  // namespace

std::string RawMouse::id_for(HANDLE device) {
    if (!device) return "";
    std::lock_guard<std::mutex> lk(dev_mu_);
    auto it = ids_.find(device);
    if (it != ids_.end()) return it->second;
    const std::wstring path = device_path(device);
    const std::string id = fnv_hex(path.empty() ? std::to_wstring(uintptr_t(device)) : path);
    ids_[device] = id;
    if (!known_.count(id)) {
        MouseDevice d;
        d.id = id;
        d.path = narrow(path);
        d.vid_pid = vid_pid_from_path(path);
        d.name = product_string(path);
        known_[id] = d;
    }
    return id;
}

std::vector<MouseDevice> RawMouse::devices() {
    UINT n = 0;
    GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
    std::vector<RAWINPUTDEVICELIST> list(n);
    if (n) n = GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST));
    std::vector<MouseDevice> out;
    if (n == UINT(-1)) return out;
    for (UINT i = 0; i < n; ++i) {
        if (list[i].dwType != RIM_TYPEMOUSE) continue;
        const std::string id = id_for(list[i].hDevice);
        std::lock_guard<std::mutex> lk(dev_mu_);
        MouseDevice d = known_[id];
        std::wstring wpath = widen(d.path);
        std::transform(wpath.begin(), wpath.end(), wpath.begin(), ::towupper);
        // Remote Desktop and some virtual devices report absolute positions.
        if (wpath.find(L"RDP_MOU") != std::wstring::npos || wpath.find(L"ROOT#") != std::wstring::npos) d.absolute = true;
        if (d.name.empty()) d.name = d.vid_pid.empty() ? "HID-compliant mouse" : "HID-compliant mouse " + d.vid_pid;
        out.push_back(d);
    }
    return out;
}

std::optional<MouseDevice> RawMouse::device(const std::string& id) {
    for (auto& d : devices())
        if (d.id == id) return d;
    return std::nullopt;
}

bool RawMouse::start(std::string* error) {
    if (active_) return true;
    // Resolve device names now, so the first reports are not delayed by HID string lookups.
    (void)devices();
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    start_error_.clear();
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.clear();
    }
    thread_ = std::thread([this] { thread_main(); });
    WaitForSingleObject(ready_, 3000);
    CloseHandle(ready_);
    ready_ = nullptr;
    if (!start_error_.empty()) {
        if (thread_.joinable()) thread_.join();
        if (error) *error = start_error_;
        return false;
    }
    active_ = true;
    return true;
}

void RawMouse::stop() {
    if (!thread_.joinable()) {
        active_ = false;
        return;
    }
    active_ = false;
    const DWORD tid = thread_id_.load();
    if (tid) PostThreadMessageW(tid, kStopMessage, 0, 0);
    thread_.join();
    std::lock_guard<std::mutex> lk(mu_);
    queue_.clear();
}

void RawMouse::drain(std::vector<RawEvent>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    out.insert(out.end(), queue_.begin(), queue_.end());
    queue_.clear();
}

LRESULT CALLBACK RawMouse::wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<RawMouse*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) {
        if (msg == WM_INPUT) {
            self->on_input(reinterpret_cast<HRAWINPUT>(lp));
            return DefWindowProcW(h, msg, wp, lp);
        }
        if (msg == WM_INPUT_DEVICE_CHANGE) {
            if (wp == GIDC_REMOVAL) {  // handle values can be reused by the next device that is plugged in
                std::lock_guard<std::mutex> lk(self->dev_mu_);
                self->ids_.erase(reinterpret_cast<HANDLE>(lp));
            }
            self->device_changes_.fetch_add(1);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void RawMouse::on_input(HRAWINPUT h) {
    const int64_t t = qpc_now_us();
    alignas(8) BYTE buf[sizeof(RAWINPUT) + 64];
    UINT size = sizeof buf;
    if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
    const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf);
    if (ri->header.dwType != RIM_TYPEMOUSE) return;
    // Only while FitSuite is the foreground window, or during live tuning while a game is in front.
    if (!in_games_.load() && GetForegroundWindow() != main_) return;
    const RAWMOUSE& m = ri->data.mouse;
    RawEvent e;
    e.t_us = t;
    e.injected = ri->header.hDevice == nullptr;  // SendInput and similar have no device handle
    e.device_id = e.injected ? std::string() : id_for(ri->header.hDevice);
    e.absolute = (m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;
    if (!e.absolute) {
        e.dx = m.lLastX;
        e.dy = m.lLastY;
    }
    if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) e.buttons |= kLeftDown;
    if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) e.buttons |= kLeftUp;
    if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) e.buttons |= kRightDown;
    if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP) e.buttons |= kRightUp;
    if (e.dx == 0 && e.dy == 0 && e.buttons == 0 && !e.injected && !e.absolute) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (queue_.size() < 200000) queue_.push_back(e);
}

void RawMouse::thread_main() {
    thread_id_ = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = &RawMouse::wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FitSuiteRawInput";
    RegisterClassExW(&wc);  // fails harmlessly if already registered
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!w) {
        start_error_ = "could not create the input window (error " + std::to_string(GetLastError()) + ")";
        SetEvent(ready_);
        thread_id_ = 0;
        return;
    }
    SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;  // generic desktop
    rid.usUsage = 0x02;      // mouse
    // INPUTSINK is required for a message-only window on its own thread; reports are filtered to
    // the foreground case in on_input(). Legacy mouse messages keep working for the UI.
    rid.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    rid.hwndTarget = w;
    if (!RegisterRawInputDevices(&rid, 1, sizeof rid)) {
        start_error_ = "RegisterRawInputDevices failed (error " + std::to_string(GetLastError()) + ")";
        DestroyWindow(w);
        SetEvent(ready_);
        thread_id_ = 0;
        return;
    }
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // create the thread queue before signalling
    SetEvent(ready_);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kStopMessage) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    rid.dwFlags = RIDEV_REMOVE;
    rid.hwndTarget = nullptr;  // required for RIDEV_REMOVE
    RegisterRawInputDevices(&rid, 1, sizeof rid);
    DestroyWindow(w);
    thread_id_ = 0;
}

}  // namespace fitapp::win32
