// Windows implementation of the FitSuite platform services.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "platform.hpp"

namespace fitapp::win32 {

// Posted by request_quit(): the main window ends FitSuite (same as "Quit" in the notification area).
inline constexpr UINT kMsgQuitApp = WM_APP + 3;

std::string narrow(const std::wstring& w);
std::wstring widen(const std::string& s);
int64_t qpc_now_us();

// ---- raw input ----------------------------------------------------------------------------------
// A dedicated thread owns a message-only window registered for raw mouse input. Reports are
// timestamped with QueryPerformanceCounter the moment they arrive. Registration exists only
// between start() and stop(), and reports are discarded whenever FitSuite is not the foreground
// window, so FitSuite never listens to the mouse in the background.
class RawMouse : public IRawMouse {
public:
    explicit RawMouse(HWND main_window) : main_(main_window) {}
    ~RawMouse() override { stop(); }
    bool start(std::string* error) override;
    void stop() override;
    bool active() const override { return active_.load(); }
    void drain(std::vector<RawEvent>& out) override;
    std::vector<MouseDevice> devices() override;
    std::optional<MouseDevice> device(const std::string& id) override;
    int64_t now_us() const override { return qpc_now_us(); }
    uint64_t device_change_counter() const override { return device_changes_.load(); }
    void set_capture_in_games(bool on) override { in_games_.store(on); }

private:
    void thread_main();
    static LRESULT CALLBACK wndproc(HWND, UINT, WPARAM, LPARAM);
    void on_input(HRAWINPUT h);
    std::string id_for(HANDLE device);

    HWND main_;
    std::thread thread_;
    std::atomic<bool> active_{false};
    std::atomic<bool> in_games_{false};  // live tuning: keep reports while a game is in front
    std::atomic<DWORD> thread_id_{0};
    std::atomic<uint64_t> device_changes_{0};
    std::mutex mu_;
    std::vector<RawEvent> queue_;
    std::map<HANDLE, std::string> ids_;      // device handle -> stable id
    std::map<std::string, MouseDevice> known_;
    std::mutex dev_mu_;
    HANDLE ready_ = nullptr;
    std::string start_error_;
};

// ---- PresentMon ---------------------------------------------------------------------------------
class PresentMonRunner : public IPresentMon {
public:
    explicit PresentMonRunner(std::filesystem::path config_dir);
    ~PresentMonRunner() override;
    PresentMonStatus status() override;
    void set_path(const std::string& path) override;
    bool start(const CaptureRequest& req, std::string* error) override;
    CaptureStatus poll() override;
    void cancel() override;

private:
    std::filesystem::path find_executable() const;
    std::filesystem::path config_dir_;
    std::filesystem::path user_path_;
    HANDLE process_ = nullptr;
    CaptureRequest req_;
    std::filesystem::path log_;
    int64_t started_us_ = 0;
    bool finished_reported_ = false;
};

// ---- platform -----------------------------------------------------------------------------------
class Platform : public IPlatform {
public:
    explicit Platform(HWND hwnd);
    ~Platform() override;

    std::string name() const override { return os_name_; }
    bool dev_harness() const override { return false; }
    std::filesystem::path data_dir() override { return data_dir_; }
    IRawMouse& mouse() override { return mouse_; }
    IPresentMon& presentmon() override { return presentmon_; }
    fit::PcInfo detect_pc() override;
    std::vector<DisplayInfo> displays() override;
    bool set_refresh_temporary(const std::string& device, double hz, std::string* error) override;
    bool restore_display(const std::string& device, std::string* error) override;
    bool set_refresh_persistent(const std::string& device, double hz, std::string* error) override;
    bool window_focused() const override;
    void set_cursor_captured(bool captured) override;
    void set_fullscreen(bool fullscreen) override;
    void set_render_mode(RenderMode mode, double limiter_fps) override;
    FrameTiming frame_timing() const override { return timing_; }
    std::optional<std::filesystem::path> open_file_dialog(const std::string& title, const std::string& filter_desc,
                                                          const std::string& pattern) override;
    std::optional<std::filesystem::path> save_file_dialog(const std::string& title, const std::string& default_name,
                                                          const std::string& filter_desc, const std::string& pattern) override;
    void open_folder(const std::filesystem::path& p) override;
    void open_url(const std::string& url) override;
    std::string probe_ddcci(const std::string& device) override;
    uint64_t start_program(const std::filesystem::path& exe, const std::vector<std::filesystem::path>& args,
                           std::string* error) override;
    std::optional<int> program_exit_code(uint64_t token) override;
    std::string foreground_app() override;
    std::vector<std::pair<std::string, std::string>> running_apps() override;
    bool autostart_enabled() override;
    bool set_autostart(bool on, std::string* error) override;
    bool autostart_blocked() override;
    void unblock_autostart() override;
    void set_keep_running(bool on) override { keep_running_ = on; }
    bool keep_running() const { return keep_running_; }
    std::optional<bool> system_dark_mode() override;
    void set_dark_title_bar(bool dark) override;
    // Windows sent WM_SETTINGCHANGE: the app mode (light/dark) may have changed.
    void on_setting_change() { theme_known_ = false; }
    // Hidden in the notification area: efficiency mode, lower priority, memory trimmed.
    void set_background_mode(bool on);
    bool rawaccel_driver_present() override;
    std::string rawaccel_settings_hint() override { return ""; }
    uint64_t start_download(const std::string& url, const std::filesystem::path& dest, std::string* error) override;
    DownloadStatus download_status(uint64_t token) override;
    void cancel_download(uint64_t token) override;
    uint64_t start_unzip(const std::filesystem::path& zip, const std::filesystem::path& dest, std::string* error) override;
    uint64_t start_elevated(const std::filesystem::path& exe, std::string* error) override;
    std::vector<std::filesystem::path> find_rawaccel_folders() override;
    bool vc_runtime_installed() override;
    fit::WindowsGameSettings windows_game_settings() override;
    std::filesystem::path programs_dir() override;
    std::filesystem::path self_exe() override;
    bool apply_update(const std::filesystem::path& new_exe, std::string* error) override;
    void request_quit() override;
    bool restart_pc(std::string* error) override;
    void stop_downloads();  // on exit: cancels and waits for running downloads
    struct Download;        // net_win32.cpp

    // Called by the main loop.
    RenderMode render_mode() const { return mode_; }
    double limiter_fps() const { return limiter_fps_; }
    bool cursor_captured() const { return cursor_captured_; }
    void update_timing(const FrameTiming& t) { timing_ = t; }
    void on_focus_change(bool focused) { focused_ = focused; }
    void reapply_cursor_clip();
    // Called from the window procedure the moment FitSuite stops being the active window: frees the
    // pointer at once. The test itself is paused between frames (App::on_focus_lost).
    void release_capture_now();

private:
    HWND hwnd_;
    std::filesystem::path data_dir_;
    std::string os_name_;
    RawMouse mouse_;
    PresentMonRunner presentmon_;
    RenderMode mode_ = RenderMode::Ui;
    double limiter_fps_ = 0.0;
    bool cursor_captured_ = false;
    bool fullscreen_ = false;
    WINDOWPLACEMENT saved_placement_{sizeof(WINDOWPLACEMENT)};
    LONG_PTR saved_style_ = 0;
    std::atomic<bool> focused_{true};
    FrameTiming timing_;
    std::map<std::string, std::string> ddc_cache_;  // device -> "supported"/"unsupported"
    std::vector<std::string> temporary_changed_;
    std::map<uint64_t, HANDLE> programs_;
    uint64_t next_program_ = 0;
    std::map<uint64_t, std::shared_ptr<Download>> downloads_;
    uint64_t next_download_ = 0;
    bool keep_running_ = false;
    bool theme_known_ = false, system_dark_ = false;
    bool background_mode_ = false;
    HWND last_fg_ = nullptr;
    DWORD last_fg_pid_ = 0;
    std::string last_fg_exe_;
};

// System information helpers (sysinfo.cpp).
std::string os_display_name();
fit::PcInfo query_pc_info();
// Display helpers (display.cpp). Enumeration never talks to the monitor; DDC/CI results come from
// the cache filled by probe_ddcci().
std::vector<DisplayInfo> enumerate_displays(HWND window, const std::map<std::string, std::string>& ddc_cache);
std::string probe_ddcci_device(const std::string& device);
bool change_refresh(const std::string& device, double hz, bool persistent, std::string* error);
bool restore_registry_mode(const std::string& device, std::string* error);
// Exact refresh rate of the monitor that shows `window` (e.g. 239.76), 0 if unknown.
double window_refresh_hz(HWND window);

}  // namespace fitapp::win32
