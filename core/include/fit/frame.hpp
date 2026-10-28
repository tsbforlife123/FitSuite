// FrameFit: PresentMon CSV parsing, frame-time statistics and run comparison.
//
// Units: every time is in milliseconds unless the name says otherwise. Frame time is the interval
// between consecutive presents of the game's swap chain. It is *not* mouse-to-photon latency.
#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fit/stats.hpp"

namespace fit {

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct FrameRecord {
    double interval_ms = kNaN;             // present-to-present (or CPU start-to-start for 2.x "FrameTime")
    double displayed_interval_ms = kNaN;   // how long a frame stayed on screen (display-side pacing)
    int displayed = -1;                    // 1 displayed, 0 dropped, -1 unknown
    double cpu_busy_ms = kNaN, cpu_wait_ms = kNaN;
    double gpu_busy_ms = kNaN, gpu_time_ms = kNaN, gpu_latency_ms = kNaN;
    double display_latency_ms = kNaN;      // frame CPU start → displayed (2.x DisplayLatency)
    double until_displayed_ms = kNaN;      // Present() call → displayed
    double pc_latency_ms = kNaN;           // PC Latency (needs game instrumentation)
    double click_to_photon_ms = kNaN;      // OS-observed mouse click → displayed (excludes mouse/USB/panel)
    double all_input_to_photon_ms = kNaN;
};

struct FrameCapture {
    std::string source_format;     // which PresentMon CSV flavour was detected
    std::string application;
    uint32_t process_id = 0;
    std::string swap_chain;
    std::string runtime;
    std::string present_mode;      // most common value
    int sync_interval = -1;
    int allows_tearing = -1;
    std::vector<FrameRecord> frames;
    std::map<std::string, std::string> column_used;  // metric -> CSV column it was read from
    std::vector<std::string> other_applications;     // other processes present in the file
    std::vector<std::string> warnings;
};

struct CsvParseOptions {
    std::string process_name;  // e.g. "VALORANT-Win64-Shipping.exe"; empty = process with most frames
};

std::optional<FrameCapture> parse_presentmon_csv(std::string_view text, const CsvParseOptions& opt, std::string* error);

struct StutterParams {
    double ratio = 2.0;          // a stutter frame takes at least 2× the recent median frame time...
    double min_excess_ms = 4.0;  // ...and at least 4 ms longer than it
    double window_s = 0.5;       // recent median over the previous 0.5 s of frames
};

struct FrameStats {
    size_t n_frames = 0;
    double duration_s = 0.0;
    double avg_fps = 0.0;        // frames / total time  (= 1000 / mean frame time)
    double ft_mean_ms = 0.0;
    double ft_p50_ms = 0.0, ft_p90_ms = 0.0, ft_p95_ms = 0.0, ft_p99_ms = 0.0, ft_p999_ms = 0.0, ft_max_ms = 0.0;
    double low1_fps = 0.0;       // 1% low: 1000 / mean of the slowest 1% of frame times
    double low01_fps = 0.0;      // 0.1% low, same definition
    double p99_fps = 0.0;        // 1000 / 99th-percentile frame time
    int stutter_count = 0;
    double stutters_per_min = 0.0;
    std::vector<size_t> stutter_frames;

    // Only present when the CSV has the data (NaN otherwise).
    double dropped_pct = kNaN;
    double displayed_fps = kNaN;
    double displayed_p99_ms = kNaN;
    double cpu_busy_p50_ms = kNaN, gpu_busy_p50_ms = kNaN;
    double display_latency_p50_ms = kNaN, display_latency_p95_ms = kNaN;
    double until_displayed_p50_ms = kNaN;
    double pc_latency_p50_ms = kNaN;
    double click_to_photon_p50_ms = kNaN;
    // The latency figure used for comparisons and which definition it follows:
    // "frame start to display" (2.x DisplayLatency) or "present to display" (MsUntilDisplayed).
    double latency_p50_ms = kNaN;
    std::string latency_definition;
    std::string bound_hint;      // "", "gpu", "cpu", "cap-or-sync"
};

FrameStats compute_frame_stats(const FrameCapture& cap, const StutterParams& p = {});

// ---- run-to-run comparison -----------------------------------------------------------------

struct RunMetrics {
    double avg_fps = kNaN;
    double low1_fps = kNaN;
    double p99_ms = kNaN;
    double stutters_per_min = kNaN;
    double latency_p50_ms = kNaN;  // display latency when available
    std::string latency_definition;
};
RunMetrics run_metrics(const FrameStats& s);

struct ConfigRuns {
    std::string id;
    std::string label;
    std::vector<RunMetrics> runs;
};

enum class MetricOutcome { Insufficient, Better, Worse, NoDifference };
const char* to_string(MetricOutcome o);

struct MetricComparison {
    std::string metric;        // "avg_fps", "low1_fps", "p99_ms", "stutters_per_min", "latency_p50_ms"
    std::string label;         // display name with unit
    bool higher_is_better = true;
    int n_a = 0, n_b = 0;
    double a_mean = kNaN, b_mean = kNaN;
    double a_cv = kNaN, b_cv = kNaN;  // run-to-run coefficient of variation
    double diff = kNaN;               // b - a
    double diff_pct = kNaN;
    Interval ci{kNaN, kNaN};          // 95% Welch interval for diff
    MetricOutcome outcome = MetricOutcome::Insufficient;
};

struct CompareThresholds {
    double avg_fps_pct = 3.0;
    double low1_pct = 5.0;
    double p99_pct = 5.0;
    double stutter_abs_per_min = 1.0;
    double latency_pct = 5.0;
    double noisy_cv_avg_fps = 0.03;
    double noisy_cv_low = 0.08;
    int min_runs = 3;
};

struct ConfigComparison {
    std::vector<MetricComparison> metrics;
    bool noisy = false;        // run-to-run variation too large for the differences being claimed
    bool preliminary = false;  // fewer than min_runs on either side
    std::vector<std::string> notes;
    std::string summary;
};

ConfigComparison compare_configs(const ConfigRuns& baseline, const ConfigRuns& candidate, const CompareThresholds& t = {});

enum class FrameGoal { Consistency, Throughput, Latency };

struct FrameRecommendation {
    int best = 0;              // index into the configs (0 = baseline)
    bool reliable = false;     // best differs from baseline beyond noise and thresholds
    std::string reason;
};

// configs[0] is the baseline.
FrameRecommendation recommend_config(const std::vector<ConfigRuns>& configs, FrameGoal goal, const CompareThresholds& t = {});

}  // namespace fit
