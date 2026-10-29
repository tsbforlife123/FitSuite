// MouseFit trial segmentation and per-trial / per-direction metrics.
//
// A trial is split into: reaction (target onset → movement onset), the *primary submovement*
// (the first flick), corrections, and the final hit. Over/undershoot and angular error are measured
// where the first flick ends, before any correction — measuring only at the click would hide the
// original aiming error.
#pragma once

#include <string>
#include <vector>

#include "fit/mouse_types.hpp"

namespace fit {

struct SegmentParams {
    double resample_ms = 1.0;
    double smooth_sigma_ms = 8.0;   // Gaussian smoothing of velocity
    double onset_frac = 0.05;       // onset: speed rises above 5% of peak
    double stop_frac = 0.10;        // end: speed falls below 10% of peak
    double reaccel_min_frac = 0.50; // end: a speed minimum below 50% of peak...
    double reaccel_rise_frac = 0.10;//      ...followed by a rise of 10% of peak (a corrective submovement)
    double reaccel_window_ms = 120.0;
    double min_peak_deg_s = 30.0;   // below this the trial has no real flick
    double anticipation_ms = 100.0; // onset earlier than this after target onset = guessed
    double max_gap_ms = 60.0;       // longer silence in the report stream during the flick = capture problem
};

enum class FlickClass : uint8_t { Undershoot, OnTarget, Overshoot };
enum class PrimaryEndRule : uint8_t { None, Reversal, Stop, Reacceleration, Click, WindowEnd };

const char* to_string(FlickClass c);
const char* to_string(PrimaryEndRule r);

struct TrialMetrics {
    bool valid = false;
    std::string invalid_reason;   // empty when valid

    TargetSpec spec;
    double target_dist_counts = 0.0;
    double target_angle_rad = 0.0;  // screen convention

    double reaction_ms = 0.0;       // target onset → movement onset
    double primary_ms = 0.0;        // movement onset → end of first flick
    double correction_ms = 0.0;     // end of first flick → hit (hits only)
    double acquisition_ms = 0.0;    // target onset → hit (timeouts: the timeout duration)
    bool hit = false;
    bool first_click_hit = false;
    bool ended_by_miss = false;     // round-based tests: the target disappeared after a missed click
    int misses = 0;                 // clicks before the hit (or all clicks on a timeout)

    Vec2 endpoint_counts;           // first-flick endpoint, output space, relative to the start
    double radial_ratio = 0.0;      // (endpoint · target direction) / target distance
    double radial_error_pct = 0.0;  // + overshoot, − undershoot
    double angle_error_deg = 0.0;   // + = endpoint rotated counter-clockwise on screen
    double endpoint_error_deg = 0.0;// distance from endpoint to target centre, game degrees
    // Shape of the first flick's path (movement onset → end of the first flick).
    // curve_pct: the furthest the path bows sideways from the straight line between its start and
    // end, as % of that line's length; + = to the left as you look along the flick (counter-clockwise
    // on screen), − = to the right. straightness_pct: straight-line distance ÷ path length (100 = straight).
    double curve_pct = 0.0;
    double straightness_pct = 100.0;
    FlickClass flick_class = FlickClass::OnTarget;
    PrimaryEndRule end_rule = PrimaryEndRule::None;
    double peak_speed_deg_s = 0.0;
    double start_pitch_deg = 0.0;
};

TrialMetrics analyze_trial(const Trial& trial, const GameMapping& mapping, const SegmentParams& params = {});

// Estimated report rate (Hz) from report intervals while the mouse is moving; 0 if unknown.
double estimate_polling_hz(const std::vector<Trial>& trials);

// Per-direction aggregates for display (scored, valid trials only).
struct DirectionSummary {
    double dir_deg = 0.0;           // nominal direction (nearest of the plan's directions)
    int n = 0;
    double mean_radial_error_pct = 0.0;
    double sd_radial_error_pct = 0.0;   // repeatability
    double mean_angle_error_deg = 0.0;
    double sd_angle_error_deg = 0.0;    // repeatability
    double median_acquisition_ms = 0.0;
    double median_correction_ms = 0.0;
    double first_click_hit_rate = 0.0;
    double mean_curve_pct = 0.0;         // + bows left, − bows right (see TrialMetrics::curve_pct)
    double mean_straightness_pct = 100.0;
    int n_under = 0, n_on = 0, n_over = 0;
};

struct OverallSummary {
    int n_trials = 0;
    int n_valid = 0;
    int n_timeouts = 0;
    int n_invalid = 0;
    double median_reaction_ms = 0.0;
    double median_acquisition_ms = 0.0;
    double median_correction_ms = 0.0;
    double first_click_hit_rate = 0.0;
    double mean_abs_radial_error_pct = 0.0;
    double mean_abs_angle_error_deg = 0.0;
    double sd_radial_error_pct = 0.0;
    double sd_angle_error_deg = 0.0;
    int n_under = 0, n_on = 0, n_over = 0;
};

std::vector<DirectionSummary> summarize_by_direction(const std::vector<TrialMetrics>& m, int directions = 8);
OverallSummary summarize_overall(const std::vector<TrialMetrics>& m);

}  // namespace fit
