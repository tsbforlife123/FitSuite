// MouseFit blinded comparison analysis (baseline vs candidate).
//
// Design: counterbalanced blocks (ABBA BAAB), fresh target directions/distances, the player is not
// told which block is which. Analysis: two-stage bootstrap (blocks, then trials within blocks) so
// block-to-block drift widens the uncertainty instead of being mistaken for an effect, plus a
// block-pair consistency check. Speed and accuracy are evaluated together: a candidate is only
// called better when it is faster *and* not less accurate.
#pragma once

#include <string>
#include <vector>

#include "fit/mouse_analysis.hpp"
#include "fit/stats.hpp"

namespace fit {

struct CompareOptions {
    int bootstrap_reps = 2000;
    uint64_t seed = 7;
    double hit_rate_margin = 0.05;      // candidate first-click hit rate may not drop by more than 5 points
    double endpoint_error_margin = 0.10;// ...nor first-flick endpoint error grow by more than 10%
    int min_valid_per_condition = 40;
    double min_pair_consistency = 0.75; // share of block pairs that must favour the candidate
    SegmentParams segment{};
};

struct ConditionSummary {
    int n_trials = 0;
    int n_valid = 0;
    int n_timeouts = 0;
    int n_blocks = 0;
    double geo_mean_acquisition_ms = 0.0;  // exp(mean log acquisition time); timeouts count as their duration
    double median_acquisition_ms = 0.0;
    double first_click_hit_rate = 0.0;
    double mean_endpoint_error_deg = 0.0;
    double mean_abs_radial_error_pct = 0.0;
    double mean_abs_angle_error_deg = 0.0;
    double median_correction_ms = 0.0;
};

enum class Verdict { InsufficientData, Improved, NoReliableDifference, Worse };
const char* to_string(Verdict v);

struct ComparisonResult {
    bool ok = false;
    std::string message;
    ConditionSummary baseline, candidate;

    double time_ratio = 1.0;          // candidate / baseline: geometric mean aim time (movement onset → hit), layout-adjusted
    Interval time_ratio_ci{1.0, 1.0}; // 95%
    double hit_rate_diff = 0.0;       // candidate - baseline
    Interval hit_rate_diff_ci;
    double endpoint_error_ratio = 1.0;
    Interval endpoint_error_ratio_ci{1.0, 1.0};
    int block_pairs = 0;
    int block_pairs_candidate_faster = 0;

    Verdict verdict = Verdict::InsufficientData;
    bool recommend_candidate = false;
    std::string explanation;              // plain language, shown to the user
    std::vector<std::string> details;
};

// Only validation sessions are accepted. Warm-up and positioning trials are ignored.
ComparisonResult compare_session(const Session& validation, const CompareOptions& opt = {});

}  // namespace fit
