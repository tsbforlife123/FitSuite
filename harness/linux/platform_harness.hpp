// Linux development harness platform. NOT a product build: it detects no hardware, its "mouse" is a
// simulated player and its display is virtual. Everything it produces is labelled "simulated".
#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <string>

#include "platform.hpp"

namespace fitapp {

class HarnessMouse : public IRawMouse {
public:
    explicit HarnessMouse(const int64_t* clock) : clock_(clock) {}
    bool start(std::string*) override { active_ = true; return true; }
    void stop() override { active_ = false; queue_.clear(); }
    bool active() const override { return active_; }
    void drain(std::vector<RawEvent>& out) override;
    std::vector<MouseDevice> devices() override;
    std::optional<MouseDevice> device(const std::string& id) override;
    int64_t now_us() const override { return *clock_; }
    uint64_t device_change_counter() const override { return 0; }
    void push(const RawEvent& e);  // keeps the queue sorted by time
    size_t queued() const { return queue_.size(); }

private:
    const int64_t* clock_;
    bool active_ = false;
    std::deque<RawEvent> queue_;
};

class HarnessPresentMon : public IPresentMon {
public:
    PresentMonStatus status() override;
    void set_path(const std::string&) override {}
    bool start(const CaptureRequest&, std::string* error) override;
    CaptureStatus poll() override { return {}; }
    void cancel() override {}
};

class HarnessPlatform : public IPlatform {
public:
    explicit HarnessPlatform(std::filesystem::path data_dir);
    std::string name() const override { return "Linux development harness"; }
    bool dev_harness() const override { return true; }
    std::filesystem::path data_dir() override { return data_dir_; }
    IRawMouse& mouse() override { return mouse_; }
    IPresentMon& presentmon() override { return presentmon_; }
    fit::PcInfo detect_pc() override;
    std::vector<DisplayInfo> displays() override;
    bool set_refresh_temporary(const std::string& device, double hz, std::string* error) override;
    bool restore_display(const std::string& device, std::string* error) override;
    bool set_refresh_persistent(const std::string& device, double hz, std::string* error) override;
    bool window_focused() const override { return focused_; }
    void set_cursor_captured(bool c) override { cursor_captured_ = c; }
    void set_fullscreen(bool f) override { fullscreen_ = f; }
    void set_render_mode(RenderMode m, double limiter_fps) override { mode_ = m; limiter_ = limiter_fps; }
    FrameTiming frame_timing() const override;
    std::optional<std::filesystem::path> open_file_dialog(const std::string&, const std::string&, const std::string&) override;
    std::optional<std::filesystem::path> save_file_dialog(const std::string&, const std::string&, const std::string&,
                                                          const std::string&) override;
    void open_folder(const std::filesystem::path&) override {}
    void open_url(const std::string&) override {}
    // Raw Accel stand-in: "running" writer.exe only records which file it was given and finishes at once.
    uint64_t start_program(const std::filesystem::path& exe, const std::vector<std::filesystem::path>& args,
                           std::string* error) override;
    std::optional<int> program_exit_code(uint64_t token) override {
        auto it = exit_codes_.find(token);
        if (it != exit_codes_.end()) return it->second;
        return writer_exit_code;
    }
    // Setup and updates: downloads come from local files registered with fake_downloads; unzip runs
    // the system's unzip; "elevated" installers only record that they ran.
    uint64_t start_download(const std::string& url, const std::filesystem::path& dest, std::string* error) override;
    DownloadStatus download_status(uint64_t token) override;
    uint64_t start_unzip(const std::filesystem::path& zip, const std::filesystem::path& dest, std::string* error) override;
    uint64_t start_elevated(const std::filesystem::path& exe, std::string* error) override;
    std::vector<std::filesystem::path> find_rawaccel_folders() override;
    bool vc_runtime_installed() override { return vc_runtime; }
    std::filesystem::path programs_dir() override { return data_dir() / "programs"; }
    std::filesystem::path self_exe() override { return data_dir() / "FitSuite.exe"; }
    bool apply_update(const std::filesystem::path& new_exe, std::string* error) override;
    void request_quit() override { quit_requested = true; }
    std::string foreground_app() override { return foreground; }
    std::vector<std::pair<std::string, std::string>> running_apps() override { return running; }
    bool autostart_enabled() override { return autostart; }
    bool set_autostart(bool on, std::string*) override {
        autostart = on;
        if (!on) autostart_blocked_flag = false;
        return true;
    }
    bool autostart_blocked() override { return autostart && autostart_blocked_flag; }
    void unblock_autostart() override { autostart_blocked_flag = false; }
    void set_keep_running(bool on) override { keep_running = on; }
    std::optional<bool> system_dark_mode() override { return system_dark; }
    void set_dark_title_bar(bool dark) override { title_bar_dark = dark; }
    bool rawaccel_driver_present() override { return rawaccel_present; }
    std::string rawaccel_settings_hint() override { return ""; }

    // Harness controls.
    std::string foreground = "fitsuite.exe";  // the harness window is FitSuite
    std::vector<std::pair<std::string, std::string>> running;
    bool autostart = false, keep_running = false, rawaccel_present = false;
    bool autostart_blocked_flag = false;  // as if switched off in Task Manager → Startup apps
    std::map<std::string, std::filesystem::path> fake_downloads;  // url -> local file
    bool vc_runtime = true, quit_requested = false;
    std::vector<std::string> elevated_log, update_log;
    int elevated_exit_code = 0;
    bool system_dark = false, title_bar_dark = false;
    int writer_exit_code = 0;
    std::vector<std::string> writer_log;  // "<time> <file name>"
    uint64_t next_program = 0;
    int64_t clock_us = 1'000'000;
    double frame_dt_ms = 4.0;
    void set_focused(bool f) { focused_ = f; }
    void queue_dialog_result(std::filesystem::path p) { next_dialog_ = std::move(p); }
    HarnessMouse& sim_mouse() { return mouse_; }
    std::map<uint64_t, int> exit_codes_;
    std::map<uint64_t, DownloadStatus> downloads_;
    RenderMode render_mode() const { return mode_; }
    bool cursor_captured() const { return cursor_captured_; }
    double refresh_hz() const { return current_hz_; }

private:
    std::filesystem::path data_dir_;
    HarnessMouse mouse_{&clock_us};
    HarnessPresentMon presentmon_;
    bool focused_ = true;
    bool cursor_captured_ = false;
    bool fullscreen_ = false;
    RenderMode mode_ = RenderMode::Ui;
    double limiter_ = 0.0;
    double base_hz_ = 60.0;
    double current_hz_ = 60.0;
    std::optional<std::filesystem::path> next_dialog_;
};

}  // namespace fitapp
