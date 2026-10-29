// The shared setup profile: hardware, game settings, baseline, candidates and results for all three
// tools, plus the journal of settings FitSuite changed (for rollback).
//
// Every result stores the conditions it was measured under. When the current setup no longer matches
// (new mouse, different DPI, GPU driver update, …) the result is flagged "potentially outdated"
// with the reason, instead of being silently reused.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "fit/frame.hpp"
#include "fit/motion.hpp"
#include "fit/mouse_transform.hpp"

namespace fit {

inline constexpr const char* kAppVersion = "0.2.4";
inline constexpr int kSetupSchemaVersion = 1;

using Conditions = std::map<std::string, std::string>;

struct MouseHardware {
    std::string model;      // what the player typed in Settings, e.g. "Logitech G Pro X Superlight 2"
    std::string name;       // product string, e.g. "G Pro X Superlight"
    std::string device_id;  // stable hash of the device interface path
    std::string vid_pid;    // "046D:C094"
    int dpi = 800;          // user-entered (mice do not report DPI)
    double polling_hz = 0;  // measured from report intervals, 0 = unknown
    std::string source = "user";  // "detected" | "user"
};

struct GpuInfo {
    std::string name;
    std::string driver;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    double vram_gb = 0;
};

struct PcInfo {
    std::string cpu;
    std::string os;
    double ram_gb = 0;
    std::vector<GpuInfo> gpus;
    std::string source = "not detected";
};

struct DisplayHardware {
    std::string name;       // monitor friendly name
    std::string device;     // \\.\DISPLAY1
    std::string edid_id;    // manufacturer + product code, e.g. "AUS27F1"
    int width = 0, height = 0;
    double refresh_hz = 0;
    std::vector<double> modes_hz;  // refresh rates available at the current resolution
    std::string overdrive;  // user-entered OSD setting name
    std::string ddcci = "unknown";  // "supported" | "unsupported" | "unknown"
    std::string source = "not detected";
};

struct GameProfile {
    std::string id = "valorant";  // the game measured with: "valorant", or its executable name (main_game_exe)
    double sensitivity = 0.4;     // in-game value, for games FitSuite can convert (game_yaw > 0)
    double cm360 = 0.0;           // otherwise: mouse distance for a full turn, entered by the player
    double scoped_multiplier = 1.0;
    int fps_cap = 0;  // 0 = unlimited / unknown
    std::map<std::string, std::string> graphics;  // user-confirmed in-game settings
};

struct ValidationSummary {
    std::string session_id;
    std::string finished_utc;
    std::string verdict;      // to_string(Verdict)
    std::string explanation;
    double time_ratio = 1.0, time_ratio_lo = 1.0, time_ratio_hi = 1.0;
    double hit_rate_diff = 0.0;
    double strength = 1.0;    // profile strength that was validated
};

// Latest quick check of a fix (mouse_check.hpp).
struct CheckSummary {
    std::string session_id;
    std::string finished_utc;
    std::string verdict;      // to_string(CheckVerdict): "still_fits" | "changed" | "not_enough_data"
    std::string explanation;
    double max_residual_pct = 0.0;
};

struct MouseProfileRecord {
    std::string id;
    std::string label;
    std::string created_utc;
    std::string algorithm_version;
    std::string calibration_session;
    std::string status = "candidate";  // "candidate" | "active" | "discarded"
    std::string origin = "measured";   // "measured" | "simulated"
    Transform transform;               // directional.strength is the user's chosen strength
    Conditions conditions;
    std::optional<ValidationSummary> validation;
    std::optional<CheckSummary> last_check;
};

struct FrameRun {
    std::string id;
    std::string timestamp_utc;
    std::string csv_file;          // copy of the PresentMon CSV, relative to the data folder
    std::string origin = "measured";
    std::string application;
    std::string source_format;
    std::string present_mode;
    double duration_s = 0;
    RunMetrics metrics;
    double ft_p50_ms = kNaN, ft_p95_ms = kNaN, low01_fps = kNaN, dropped_pct = kNaN;
    double cpu_busy_p50_ms = kNaN, gpu_busy_p50_ms = kNaN;
    std::string bound_hint;
    Conditions conditions;
};

struct FrameConfig {
    std::string id;
    std::string label;
    bool baseline = false;
    std::map<std::string, std::string> settings;  // e.g. "fps_cap" -> "240"
    bool user_confirmed = false;  // the user confirmed these settings are active in the game
    std::vector<FrameRun> runs;
};

struct MotionSessionRecord {
    std::string id;
    std::string timestamp_utc;
    std::string origin = "measured";
    std::string test = "identification";  // "identification" | "trails"
    std::vector<MotionConfig> configs;
    std::vector<MotionResponse> responses;
    std::vector<TrailRating> ratings;
    Conditions conditions;
};

struct ResultRecord {
    std::string id;
    std::string tool;        // "mousefit" | "framefit" | "motionfit"
    std::string kind;        // "calibration", "validation", "frame-run", "motion-test", …
    std::string timestamp_utc;
    std::string summary;
    std::string origin = "measured";
    Conditions conditions;
};

struct AppliedChange {
    std::string id;
    std::string kind;        // "display.refresh_rate", "mousefit.active_profile"
    std::string target;      // e.g. \\.\DISPLAY1
    std::string before;
    std::string after;
    std::string applied_utc;
    std::string reverted_utc;
    std::string status = "applied";  // "pending" | "applied" | "reverted" | "failed"
    bool temporary = false;
};

// MouseFit: use the Raw Accel fix only while one of these games is in front.
struct GameActivation {
    bool enabled = false;  // FitSuite switches the fix on and off in the background
    std::vector<std::string> games = {"valorant-win64-shipping.exe"};  // executable names, lower case
    std::string rawaccel_settings;   // the player's Raw Accel settings.json (never changed by this mode)
    std::string rawaccel_corrected;  // the same settings with the fix (FitSuite's data folder, rewritten as needed)
    bool start_with_windows = true;  // superseded by AppSettings::start_with_windows; read once from older files
};

// A game's sensitivity, to compare how fast it turns with the VALORANT setting the fix was measured at.
struct GameSensitivity {
    double sensitivity = 0.0;  // in-game value, for games whose conversion FitSuite knows (0 = not entered)
    double cm360 = 0.0;        // or the mouse distance for a full turn, entered directly (0 = not entered)
};

// The Settings tab.
struct AppSettings {
    std::string theme = "system";      // "system" | "light" | "dark"
    bool start_with_windows = false;   // open FitSuite when you sign in to Windows
    bool start_hidden = true;          // ...straight into the notification area, without the window
    bool close_to_tray = true;         // closing the window keeps FitSuite running in the notification area
    bool show_extra_tools = false;     // the Frame rate and Monitor tabs
    int check_reminder_days = 7;       // remind to run the quick check: 7, 30 or 0 (never)
    std::string last_reminder_utc;     // when a reminder was last shown (at most one a day)
    std::map<std::string, GameSensitivity> game_sens;  // by executable name, lower case
    // Monitors the player chose to keep below their fastest refresh rate (refresh_dismiss_key()).
    std::vector<std::string> refresh_kept;
    // First-run setup: false until it was finished or skipped (files from before it count as done).
    bool setup_done = false;
    int setup_step = 0;                  // where the setup continues (e.g. after the restart Raw Accel needs)
    std::string rawaccel_folder;         // where the setup unpacked Raw Accel
    bool rawaccel_install_pending = false;  // its installer ran; the driver appears after a restart
    // Updates: checked once a day when switched on (FitSuite's public GitHub releases).
    bool update_check = true;
    std::string last_update_check_utc;
    std::string update_notified;         // version the player was last told about
    std::string notes_seen;              // version whose "What's new" notes the player has seen
};

struct SetupProfile {
    int schema_version = kSetupSchemaVersion;
    std::string app_version = kAppVersion;
    std::string id;
    std::string name = "My setup";
    std::string created_utc;
    std::string updated_utc;

    MouseHardware mouse;
    PcInfo pc;
    DisplayHardware display;
    GameProfile game;

    std::string baseline_captured_utc;
    std::map<std::string, std::string> baseline;  // snapshot of settings to restore

    std::vector<MouseProfileRecord> mouse_profiles;
    std::string active_mouse_profile;  // id, empty = baseline (no correction)
    std::vector<std::string> mouse_sessions;
    GameActivation fix_games;
    AppSettings app;

    std::vector<FrameConfig> frame_configs;
    std::vector<MotionSessionRecord> motion_sessions;
    std::string motion_selected;  // label of the chosen monitor configuration

    std::vector<ResultRecord> results;
    std::vector<AppliedChange> applied_changes;
};

SetupProfile make_default_setup();

// ---- conditions and staleness ------------------------------------------------------------------
Conditions current_conditions(const SetupProfile& p);
// Keys that matter for each tool's results.
std::vector<std::string> condition_keys(const std::string& tool);
Conditions conditions_for(const std::string& tool, const SetupProfile& p);

struct Staleness {
    bool outdated = false;
    std::vector<std::string> reasons;  // e.g. "DPI changed from 800 to 1600"
};
Staleness check_staleness(const Conditions& recorded, const Conditions& current);
std::string describe_condition_key(const std::string& key);
// Short human-readable summary, e.g. "VALORANT · sensitivity 0.4 · Logitech G Pro · 800 DPI".
std::string describe_conditions(const Conditions& c);

// Snapshot of the settings FitSuite knows about, for "restore previous configuration".
std::map<std::string, std::string> snapshot_settings(const SetupProfile& p);

}  // namespace fit
