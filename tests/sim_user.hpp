// Synthetic player for tests and the Linux development harness ONLY. It is never compiled into
// FitSuite.exe. It produces raw input reports the way a person would: a reaction delay, a
// minimum-jerk primary flick with a systematic distortion and noise, then visually guided
// corrections and a click.
#pragma once

#include <cstdint>
#include <vector>

#include "fit/mouse_transform.hpp"
#include "fit/mouse_types.hpp"
#include "fit/rng.hpp"

namespace fit::sim {

struct SimUserParams {
    // Systematic motor distortion (input space): hand movement = M × planned movement.
    double m00 = 1.0, m01 = 0.0, m10 = 0.0, m11 = 1.0;
    // Direction-specific gain (one-sided, applied after M): e.g. down_gain = 0.88 = flicks toward the
    // bottom of the screen land 12% short while upward flicks are fine. Not representable by M.
    double right_gain = 1.0, left_gain = 1.0, up_gain = 1.0, down_gain = 1.0;
    double primary_gain = 0.93;   // typical undershoot of the first flick
    // Distance-dependent landing: the first flick is scaled by (distance / 12°)^slope, so a negative
    // slope makes far targets land shorter and near ones longer (the "range effect").
    double dist_gain_slope = 0.0;
    double radial_sd = 0.07;      // multiplicative noise of the first flick
    double angle_sd_deg = 2.0;
    double reaction_ms = 230.0, reaction_sd_ms = 35.0;
    double mt_base_ms = 110.0, mt_per_bit_ms = 35.0;
    double corr_mt_ms = 140.0;
    double corr_noise = 0.15;
    double click_delay_ms = 70.0;
    double early_click_prob = 0.25;  // clicks right after the flick when close (may miss)
    double poll_hz = 1000.0;
    double timeout_ms = 3000.0;
    double speed_factor = 1.0;       // multiplies every duration (models practice/fatigue drift)
    // Round-based tests: the first click ends the target (a miss makes it disappear), and the hand
    // may still be settling from the previous target when the next one appears.
    bool single_click = false;
    double settle_deg = 0.0;         // size of that leftover movement (random direction), degrees
    double settle_ms = 60.0;
    // Tracking: reaction lag and how strongly the player corrects the remaining error.
    double track_lag_ms = 130.0;
    double track_correction = 4.0;   // per second
    double track_noise = 0.08;       // multiplicative velocity noise
};

class SimUser {
public:
    SimUser(SimUserParams p, uint64_t seed) : p_(p), rng_(seed) {}
    SimUserParams& params() { return p_; }

    // One trial exactly as the app would record it, with `active` applied to the output.
    Trial run_trial(const TargetSpec& spec, const GameMapping& map, const Transform& active, double start_yaw_deg,
                    double start_pitch_deg, int64_t t_appear_us);

    // Tracking: raw counts for the next `dt_s` seconds, given the target's position and velocity
    // (degrees, degrees per second) and the crosshair's position. Call once per frame.
    Vec2 track_step(double now_s, double dt_s, double target_yaw, double target_pitch, double target_vyaw,
                    double target_vpitch, double aim_yaw, double aim_pitch, const GameMapping& map);

private:
    SimUserParams p_;
    Rng rng_;
    struct Seen {
        double t, ty, tp, tvy, tvp, ay, ap;
    };
    std::vector<Seen> seen_;  // what the player saw, for the reaction lag
    Vec2 track_carry_{};
};

// Applies a 2x2 matrix to a vector.
inline Vec2 mul(const SimUserParams& p, Vec2 v) { return {p.m00 * v.x + p.m01 * v.y, p.m10 * v.x + p.m11 * v.y}; }

}  // namespace fit::sim
