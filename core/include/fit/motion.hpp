// MotionFit: moving-detail identification test scheduling and analysis.
//
// The player tracks a ring moving horizontally and reports where its gap points (4 choices).
// Accuracy falls as speed rises; the speed where accuracy is 75% summarises motion clarity for one
// display configuration. All results are user-observed: software cannot see the panel itself.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fit/stats.hpp"

namespace fit {

struct MotionConfig {
    std::string label;          // e.g. "240 Hz, overdrive Normal"
    double refresh_hz = 0.0;
    std::string overdrive;      // user-entered monitor setting name; empty if not applicable
    double content_fps = 0.0;   // 0 = one update per refresh; otherwise motion updates at this rate
    bool app_controlled = false;// true when FitSuite switched the refresh rate itself (blinded)
};

struct MotionTrialSpec {
    int index = 0;
    int config = 0;             // index into the configurations under test
    int block = 0;
    double speed_sw_s = 1.0;    // screen widths per second
    int orientation = 0;        // 0 right, 1 up, 2 left, 3 down
};

struct MotionResponse {
    MotionTrialSpec spec;
    int answer = -1;            // -1 = no answer / "could not tell"
    bool correct = false;
    double response_ms = 0.0;
    int dropped_frames = 0;     // detected presentation hitches during the stimulus
};

struct MotionPlanOptions {
    // Log-spaced (×√2). 2.0 is the fastest speed whose path still fits on screen during the 450 ms stimulus.
    std::vector<double> speeds_sw_s{0.25, 0.35, 0.5, 0.7, 1.0, 1.4, 2.0};
    int reps_per_speed_per_block = 2;
    int blocks_per_config = 3;  // configurations alternate block by block (A B A B …)
    uint64_t seed = 3;
};

struct MotionPlan {
    std::vector<std::vector<MotionTrialSpec>> blocks;
    std::vector<int> block_config;
};

MotionPlan make_motion_plan(int n_configs, const MotionPlanOptions& o);

struct PsychometricFit {
    bool ok = false;
    double mu = 0.0;            // log2 speed at the midpoint
    double slope = 0.5;         // spread in log2 units
    double threshold_sw_s = 0.0;// speed with 75% correct identification
    Interval threshold_ci{0.0, 0.0};
    double overall_accuracy = 0.0;
    int n = 0;
    std::vector<double> speeds, accuracy;  // per speed level (for plotting)
    std::vector<int> counts;
};

// 4-alternative model: P(correct) = 0.25 + 0.75·(1 - lapse)·logistic(-(log2 v - mu)/slope).
PsychometricFit fit_psychometric(const std::vector<MotionResponse>& responses, int config, int bootstrap_reps = 400,
                                 uint64_t seed = 11);

struct MotionComparison {
    bool ok = false;
    double threshold_ratio = 1.0;        // b / a (> 1 = b keeps details readable at higher speed)
    Interval threshold_ratio_ci{1.0, 1.0};
    double accuracy_diff = 0.0;          // b - a, pooled over speeds
    Interval accuracy_diff_ci{0.0, 0.0};
    std::string verdict;                 // "better", "worse", "no-reliable-difference", "insufficient-data"
    std::string explanation;
};

MotionComparison compare_motion(const std::vector<MotionResponse>& responses, int config_a, int config_b,
                                int bootstrap_reps = 400, uint64_t seed = 13);

// Subjective ratings from the trail-inspection test (0 = none visible … 4 = very visible).
struct TrailRating {
    int config = 0;
    int round = 0;
    int trail = 0;       // dark smearing behind moving objects (ghosting)
    int overshoot = 0;   // bright/dark halos (inverse ghosting / overshoot)
};

// Is the monitor running slower than the fastest refresh rate Windows offers at its current
// resolution? A common setup mistake (a 144 Hz monitor left at 60 Hz). Differences under 5 Hz
// (59 vs 60 Hz, for example) do not count. Windows only lists rates that the monitor, the cable and
// the graphics card all accept, so a rate missing from the list cannot be detected here.
struct RefreshAdvice {
    bool slower = false;
    double current_hz = 0.0;
    double max_hz = 0.0;
};
RefreshAdvice refresh_advice(double current_hz, const std::vector<double>& modes_hz);
// Identifies "keep this monitor at its current rate" choices; a new, faster rate asks again.
std::string refresh_dismiss_key(const std::string& monitor_id, double max_hz);

}  // namespace fit
