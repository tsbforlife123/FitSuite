// MouseFit profile fitting.
//
// Fits two correction candidates from a *calibration* session (baseline input only):
//   1. DirectionalProfile — smooth 2nd-order Fourier gain and angle correction (in-app only today).
//   2. RawAccelProfile    — rotation + Y/X ratio (optionally L/R and U/D), the subset Raw Accel can apply.
// A correction component is only kept when it is statistically distinguishable from zero and large
// enough to matter; otherwise the recommendation is to keep the baseline.
#pragma once

#include <string>
#include <vector>

#include "fit/mouse_analysis.hpp"
#include "fit/mouse_transform.hpp"
#include "fit/mouse_types.hpp"

namespace fit {

struct FitOptions {
    Bounds bounds{};
    double alpha = 0.01;               // per component (gain, angle, Raw Accel); family-wise ≈ 3%
    double prior_sd_log_gain = 0.05;   // ridge prior on each Fourier gain coefficient
    double prior_sd_angle_deg = 2.0;   // ridge prior on each Fourier angle coefficient
    int min_per_direction = 6;
    int min_total = 64;
    int min_distances = 2;
    double gross_radial_error = 0.6;   // exclude flicks off by more than 60% of the distance
    double gross_angle_error_deg = 30.0;
    double negligible_gain = 0.01;     // smaller corrections are not worth applying
    double negligible_angle_deg = 0.5;
    bool raw_accel_asymmetry = true;   // also fit L/R and U/D multipliers (separate up/down/left/right gains)
    // Speed curve: sensitivity that changes a little with hand speed, when flicks to far targets
    // (fast) land differently from flicks to near ones (slow). Raw Accel applies it as a lookup table.
    bool speed_curve = true;
    double speed_max_effect = 0.05;    // a flick at the fastest or slowest measured level changes by at most 5%
    double negligible_speed = 0.02;    // fastest vs slowest level must differ by at least 2% to be worth it
    SegmentParams segment{};
};

struct ComponentTest {
    double statistic = 0.0;
    double df = 0.0;
    double p_value = 1.0;
    bool significant = false;
};

struct FitResult {
    bool ok = false;               // false: wrong input or not enough data (see message)
    std::string message;           // plain-language explanation
    std::vector<std::string> data_problems;

    int n_trials = 0, n_used = 0, n_excluded_gross = 0, n_invalid = 0;
    std::vector<double> distances_deg;

    // Smooth directional candidate (strength 1, bounded).
    DirectionalProfile directional;
    ComponentTest gain_test, angle_test;
    bool directional_bounds_scaled = false;
    bool directional_recommended = false;

    // Raw Accel-compatible candidate.
    RawAccelProfile raw_accel;
    ComponentTest raw_accel_test;
    bool raw_accel_recommended = false;
    bool raw_accel_bounds_scaled = false;      // the fitted correction was scaled down to the bounds
    double raw_accel_gain_mismatch = 0.0;      // max |g_RA - g_dir| over directions
    double raw_accel_angle_mismatch_deg = 0.0; // max |δ_RA - δ_dir| over directions

    // Speed curve (part of the Raw Accel candidate: raw_accel.speed). Fitted from the flick rounds:
    // log(distance ratio) against log(typical peak hand speed of the flick's distance level).
    ComponentTest speed_test;
    bool speed_recommended = false;
    bool speed_bounds_scaled = false;
    double speed_exponent = 0.0;          // sensitivity ∝ speed^k between the measured levels
    double speed_slow_pct = 0.0;          // effect on a flick at the slowest level (+ = more sensitivity)
    double speed_fast_pct = 0.0;          // ...and at the fastest level
    double speed_slow_cpms = 0.0, speed_fast_cpms = 0.0;  // their typical peak speeds, counts per ms
    // In-sample: largest difference between a distance level's average and the average of all levels.
    double speed_level_bias_before_pct = 0.0, speed_level_bias_after_pct = 0.0;

    // In-sample check on the calibration flicks (not a validated result): largest per-direction
    // bias relative to the player's own average, before and after each correction.
    double max_radial_bias_before_pct = 0.0;
    double max_radial_bias_after_dir_pct = 0.0;
    double max_radial_bias_after_ra_pct = 0.0;
    double max_angle_bias_before_deg = 0.0;
    double max_angle_bias_after_dir_deg = 0.0;
    double max_angle_bias_after_ra_deg = 0.0;

    std::vector<DirectionSummary> by_direction;
    OverallSummary overall;
};

// Rejects anything that is not a calibration session; only scored baseline trials are used.
FitResult fit_profiles(const Session& calibration, const FitOptions& opt = {});

// The Raw Accel lookup table for sensitivity ∝ speed^k, normalised so a flick whose peak speed is
// v_mid keeps its distance (each flick sweeps through all speeds up to its peak, so the table is
// scaled by the average over a minimum-jerk speed profile). Points span from well below v_slow to
// above v_fast; outside them the sensitivity stays flat. Every value within ±kSpeedCurveMaxDeviation.
SpeedCurve make_speed_curve(double k, double v_mid, double v_slow, double v_fast);
// What a speed curve does to a whole minimum-jerk flick with this peak speed (counts per ms): its
// distance multiplier (1 = unchanged).
double speed_curve_flick_gain(const SpeedCurve& c, double peak_cpms);

}  // namespace fit
