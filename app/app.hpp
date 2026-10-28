// FitSuite application: navigation, shared setup profile, page state and test hosting.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fit/aim_report.hpp"
#include "fit/update.hpp"
#include "fit/frame.hpp"
#include "fit/game_activation.hpp"
#include "fit/mouse_check.hpp"
#include "fit/mouse_compare.hpp"
#include "fit/mouse_fit.hpp"
#include "fit/profile.hpp"
#include "fit/store.hpp"
#include "motion_test.hpp"
#include "mouse_test.hpp"
#include "platform.hpp"
#include "widgets.hpp"

namespace fitapp {

enum class Page { Overview, MouseFit, FrameFit, MotionFit, Profiles, Nerd, Settings };

struct Toast {
    std::string text;
    ui::Tone tone = ui::Tone::Info;
    double until = 0.0;
};

struct MouseFitUi {
    int tab = 0;                   // 0 setup, 1 calibrate, 2 candidates, 3 validate, 4 apply
    int calib_blocks = 2;  // flick rounds in a calibration (1–4)
    float radius_deg = 1.2f;
    int val_blocks = 8;
    std::optional<fit::Session> calibration;
    std::vector<fit::TrialMetrics> calib_metrics;
    std::optional<fit::AimReport> aim_report;  // round-based calibrations
    std::optional<fit::FitResult> fit;
    std::optional<fit::Session> validation;
    std::optional<fit::ComparisonResult> comparison;
    std::string selected_profile;  // profile id under review
    std::unique_ptr<MouseTest> test;
    bool identifying = false;      // "move the mouse you want to use"
    std::string identified_device;
    std::string patch_message;
    std::vector<std::string> patch_warnings;
    std::filesystem::path ra_original;   // the player's Raw Accel settings.json
    std::filesystem::path ra_generated;  // the corrected copy FitSuite wrote next to it
    bool loaded_from_disk = false;
    bool confirm_default = false;     // Nerd stats → Apply: "Make it my Raw Accel default?" shown
    bool confirm_everywhere = false;  // "Use the fix everywhere?" shown
    std::vector<std::pair<std::string, std::string>> running;  // programs with a window (exe, title)
    double running_checked_at = 0.0;
    bool show_running = false;  // "Add a running game" list is open
};

struct FrameFitUi {
    std::string selected_config;
    std::string compare_a, compare_b;
    int goal = 0;  // FrameGoal
    CaptureRequest request;
    bool capturing = false;
    std::string capture_config;
    std::map<std::string, fit::FrameCapture> captures;  // run id -> frames (for plots)
    std::string plot_run;
    char new_label[64] = "";
    char new_key[48] = "";
    char new_value[48] = "";
    char presentmon_path[512] = "";
    std::string last_error;
    CaptureState capture_state = CaptureState::Idle;
    std::string capture_message;
    PresentMonStatus pm_status;
    double pm_checked_at = 0.0;
    char simple_new_label[64] = "";
};

struct PendingDisplayChange {
    bool active = false;
    std::string device;
    double previous_hz = 0.0;
    double new_hz = 0.0;
    double deadline = 0.0;
    std::string change_id;
    std::string label;  // MotionFit setting to record as chosen when the change is kept
};

struct MotionFitUi {
    std::vector<int> refresh_pick;  // indexes into display modes
    std::vector<std::string> overdrive_names;
    char overdrive_input[64] = "";
    bool match_framefit_fps = false;
    int mode = 0;  // 0 refresh rates (blinded), 1 overdrive settings (manual)
    std::unique_ptr<MotionTest> test;
    std::string last_session;
    PendingDisplayChange pending;
};

// First-run setup: installing Raw Accel from its official release (pinned version and SHA-256).
struct RaInstall {
    enum class Step { Idle, Downloading, Unzipping, Installing, NeedsRestart, Failed };
    Step step = Step::Idle;
    uint64_t token = 0;
    uint64_t bytes = 0, total = 0;
    std::string message;               // what went wrong, or what happens next
    std::filesystem::path zip, folder;
};

// Updates from FitSuite's public GitHub releases: check, download, verify, restart into the new version.
struct Updater {
    enum class Step { Idle, Checking, UpToDate, Available, Downloading, Ready, Failed };
    Step step = Step::Idle;
    bool manual = false;               // the player pressed "Check now" (errors are shown)
    int phase = 0;                     // while downloading: 0 checksum, 1 program
    uint64_t token = 0;
    uint64_t bytes = 0, total = 0;
    std::optional<fit::ReleaseInfo> release;
    std::string expected_sha;
    std::filesystem::path exe;         // the downloaded and checked FitSuite.exe
    std::string message;
    // The "Update available" box shown when FitSuite opens.
    bool prompt_dismissed = false;     // "Not now": hidden until FitSuite is opened again
    bool install_when_ready = false;   // "Update": install and restart as soon as the download is checked
};

class App {
public:
    explicit App(IPlatform& platform);
    ~App();

    void init();
    // Builds the whole UI for one frame (between ImGui::NewFrame and ImGui::Render).
    void frame();
    void shutdown();

    bool test_running() const;
    void on_focus_lost();

    // ---- shared services for pages ----
    IPlatform& platform() { return platform_; }
    fit::Store& store() { return *store_; }
    fit::SetupProfile& setup() { return setup_; }
    // Saves the setup about a second after the last change (typing a number does not write a file
    // per keystroke); save_now() writes at once, for changes that must not be lost.
    void save();
    void save_now();
    void toast(ui::Tone tone, const std::string& text, double seconds = 5.0);
    void go(Page p) { page_ = p; }
    double time() const;
    void refresh_hardware();
    // Displays only (cheap): at start, when Windows reports a display change, and after a switch.
    void refresh_displays();
    const std::vector<DisplayInfo>& displays() const { return displays_; }
    // Monitors running below the fastest refresh rate Windows offers (fit::refresh_advice).
    struct SlowDisplay {
        DisplayInfo display;
        fit::RefreshAdvice advice;
        std::string key;  // fit::refresh_dismiss_key
        bool kept = false;  // the player chose to keep the slower rate
    };
    std::vector<SlowDisplay> slow_displays(bool include_kept = false) const;
    void keep_refresh(const std::string& key);
    static std::string display_id(const DisplayInfo& d);
    void record_result(const std::string& tool, const std::string& kind, const std::string& summary,
                       const std::string& origin);
    void capture_baseline();
    bool load_mouse_session(const std::string& id, fit::Session& out);

    // MouseFit actions
    void start_calibration();
    void start_validation(const std::string& profile_id);
    // The 90-second quick check of the chosen fix (mouse_check.hpp).
    void start_check();
    // The chosen fix (Raw Accel-compatible profile), or nullptr.
    fit::MouseProfileRecord* active_fix();
    // Days since the chosen fix was measured, blind-tested or checked; -1 without a fix.
    int days_since_fix_checked();
    bool check_due();  // per the reminder setting
    // Stops using the chosen fix: per-game switching off, Raw Accel default restored, no fix chosen.
    void remove_fix();
    void analyze_calibration();
    void on_mouse_test_done();
    fit::MouseProfileRecord* mouse_profile(const std::string& id);
    fit::SessionConditions mouse_conditions() const;

    // Raw Accel. FitSuite only ever runs Raw Accel's own writer.exe (found next to the settings file,
    // or in writer_dir) with a settings file, and records the result in the change log.
    // purpose: "apply", "default", "restore", "game-on" or "game-off".
    bool rawaccel_run_writer(const std::filesystem::path& settings, const std::string& purpose, const std::string& change_id,
                             std::string* error, const std::filesystem::path& writer_dir = {});
    bool rawaccel_busy() const { return writer_.token != 0; }
    // Saves a copy of settings.json, puts the corrected settings in its place and applies them.
    bool rawaccel_make_default(const std::filesystem::path& settings_json, const std::filesystem::path& corrected,
                               std::string* message);
    // Undoes a Raw Accel change from the change log (puts the saved copy back and re-applies it).
    bool rawaccel_restore(fit::AppliedChange& change, std::string* error);

    // MotionFit
    void start_motion_test(MotionTestConfig cfg);
    void on_motion_test_done();

    // ---- MouseFit only in your games ----
    // While on, FitSuite keeps running (in the notification area once its window is closed) and
    // switches Raw Accel between the player's own settings.json and a copy with the fix, depending on
    // which program is in front. It only reads that program's name; it never looks into the game.
    bool games_turn_on(const std::filesystem::path& settings_json, std::string* message);
    void games_turn_off();
    void games_retry();  // after a problem paused switching
    struct GamesState {
        bool enabled = false;
        bool have_fix = false;       // a Raw Accel fix is chosen (otherwise it stays off)
        std::optional<bool> fix_on;  // what Raw Accel uses now (nullopt: not known yet)
        bool switching = false;      // Raw Accel's writer is running for a switch
        bool writer_slow = false;    // the writer has been open for a while: probably showing a message
        std::string foreground;      // program in front at the last check
        std::string game;            // the game the fix is on for (while it is on)
        std::string error;           // switching is paused until the player retries
    };
    GamesState games_state() const;
    std::string tray_text() const;  // one line for the notification-area tooltip
    // Closing the window keeps FitSuite running in the notification area (setting, or needed by the
    // per-game fix).
    bool keep_running() const { return setup_.app.close_to_tray || setup_.fix_games.enabled; }
    // Windows main loop while the window is hidden (no frames then): run due work, and how long it
    // may sleep before the next call (it is also woken when another program comes to the front).
    void background_tick();
    double seconds_until_next_tick() const;

    // ---- first-run setup and updates (setup_flow.cpp) ----
    RaInstall ra_install;
    Updater updater;
    void ra_install_start();
    // A folder with Raw Accel's writer.exe (the one the setup installed, or one found on this PC).
    std::filesystem::path rawaccel_folder();
    bool rawaccel_needs_vc_runtime();
    void open_rawaccel_gui();
    void update_check(bool manual);
    void update_download();
    void update_install();
    bool update_busy() const { return updater.step == Updater::Step::Checking || updater.step == Updater::Step::Downloading; }
    // A one-time notice for the notification area when a new version is available; empty otherwise.
    std::string take_update_notice();
    void finish_setup();
    void on_foreground_changed() { games_next_check_ = 0.0; }
    // Only the per-game fix needs to hear about program switches; otherwise FitSuite is not woken by them.
    bool wants_foreground_events() const { return setup_.fix_games.enabled; }
    // A notification-area reminder to run the quick check, at most once a day and never while the fix
    // is on in a game; empty when there is nothing to say.
    std::string take_reminder();
    // "Open FitSuite when Windows starts".
    bool set_start_with_windows(bool on);
    // The startup entry is there but switched off in Task Manager (checked at most every 3 s).
    bool startup_blocked();
    // The Raw Accel settings.json the player chose (remembered between runs); empty if none.
    std::filesystem::path rawaccel_settings_path();
    void remember_rawaccel_settings_path(const std::filesystem::path& p);

    // Settings → Check my setup.
    struct DiagItem {
        bool ok = false;
        std::string what, detail;
    };
    std::vector<DiagItem> check_setup();
    // The window was closed to the notification area: tests stop, an unconfirmed display change reverts.
    void on_hidden();

    MouseFitUi mouse;
    FrameFitUi frame_ui;
    MotionFitUi motion;
    int nerd_tab = 0;  // Nerd stats: 0 MouseFit, 1 FrameFit, 2 MotionFit, 3 System
    std::vector<DiagItem> diagnostics;  // last "Check my setup" results
    std::string diagnostics_time;
    char model_input[128] = "";          // Settings: mouse model being typed
    bool model_input_loaded = false;
    std::vector<std::string> load_messages;  // shown once on Overview (e.g. recovered from backup)

    // Raw Accel's driver is installed (checked at most every 3 seconds).
    bool rawaccel_installed();

private:
    void apply_theme();
    bool theme_dark_ = false, theme_set_ = false;
    void draw_tabs();
    void draw_content(ImVec2 item_spacing);
    void draw_status_bar();
    void draw_test_overlay();
    double rawaccel_checked_at_ = 0.0;
    bool rawaccel_installed_ = false;
    bool save_pending_ = false;
    double save_due_ = 0.0;

    struct WriterJob {
        uint64_t token = 0;
        std::string purpose, change_id;
        double started = 0.0;  // clock_s()
    };
    WriterJob writer_;
    void tick_writer();
    void wait_for_writer(double seconds);

    // Monotonic seconds that keep running while the window is hidden (frames, and with them
    // ImGui's clock, stop then). Virtual time in the development harness.
    double clock_s() const { return double(platform_.mouse().now_us()) * 1e-6; }
    void tick_games();
    bool games_write_corrected(std::string* error, std::vector<std::string>* warnings = nullptr);
    bool games_tests_blocked();
    fit::ActivationSwitcher switcher_{1.0};
    std::optional<bool> games_on_;       // what Raw Accel uses (nullopt: unknown)
    std::optional<bool> games_pending_;  // the state the running writer switches to
    bool games_restore_needed_ = false;  // turned off while the fix may be on: put the own settings back
    std::string games_fg_, games_error_;
    std::string games_on_for_;  // the game that was in front when the fix was switched on
    double games_next_check_ = 0.0;

    IPlatform& platform_;
    std::unique_ptr<fit::Store> store_;
    fit::SetupProfile setup_;
    Page page_ = Page::MouseFit;
    std::vector<Toast> toasts_;
    bool initialized_ = false;
    std::vector<DisplayInfo> displays_;
    bool shut_down_ = false;
    void tick_ra_install();
    void tick_updater();
    void draw_update_prompt();
    bool checked_this_launch_ = false;  // updates are checked once each time FitSuite opens
    double rawaccel_folder_checked_at_ = -100.0;
    std::filesystem::path rawaccel_folder_cache_;  // shutdown() ran (on exit, or when Windows ends the session)
    bool autostart_blocked_ = false;
    double autostart_blocked_at_ = -1.0;
};

// Pages. The main pages are the simple views for every player; Nerd stats holds the detailed views
// (every statistic and control) of each tool.
void page_mousefit(App& app);
void page_framefit(App& app);
void page_motionfit(App& app);
void page_profiles(App& app);
void page_nerd(App& app);
void page_settings(App& app);
void overview_details(App& app);
void mousefit_details(App& app);
void framefit_details(App& app);
void motionfit_details(App& app);

// Shared helpers.
// The shooters FitSuite knows, one entry per game (some have a DirectX 12 or Vulkan executable as
// well), followed by games the player added.
struct GameChoice {
    std::string name;
    std::vector<std::string> exes;
    double yaw = 0.0;  // degrees per count at sensitivity 1, 0 = no conversion (enter cm/360)
};
std::vector<GameChoice> game_choices(const std::vector<std::string>& selected);
// Mouse distance for a full turn, in cm (0 = not known).
double valorant_cm360(const fit::SetupProfile& s);
double game_cm360(const fit::SetupProfile& s, const GameChoice& g);
// "turns 30% faster than VALORANT" ("" when unknown); *warn when it differs by more than 15%.
// brief: "turns 30% faster" / "about the same", for a column that already says "compared with VALORANT".
std::string turn_comparison(double game_cm, double valorant_cm, bool* warn, bool brief = false);
void staleness_line(App& app, const fit::Conditions& recorded, const std::string& tool);
std::vector<fit::ConfigRuns> frame_config_runs(const fit::SetupProfile& s);
void framefit_import(App& app, const std::string& config_id, const std::filesystem::path& csv, const std::string& origin);
bool framefit_start_capture(App& app, const std::string& config_id);
void framefit_tick(App& app);
bool presentmon_ready(App& app);
void tick_pending_display(App& app);
void page_setup(App& app);
// Switches a display's saved refresh rate with the 15-second keep-or-revert safety net (logged in History).
bool switch_refresh_rate(App& app, const std::string& device, double from_hz, double to_hz, const std::string& label);
// Warning with a one-click fix when a monitor runs below its fastest refresh rate.
void refresh_rate_notice(App& app);
void revert_pending_display(App& app);  // an unconfirmed refresh-rate change goes back at once
bool rawaccel_export(App& app, const fit::RawAccelProfile& correction, const std::filesystem::path& settings_json);
const fit::FrameConfig* frame_baseline(const fit::SetupProfile& s);
std::string short_time(const std::string& iso);
ui::Origin origin_of(const std::string& origin);

}  // namespace fitapp
