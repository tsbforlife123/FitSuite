#include "sim_user.hpp"

#include <algorithm>
#include <cmath>

namespace fit::sim {

namespace {

double min_jerk(double tau) {
    tau = std::clamp(tau, 0.0, 1.0);
    return tau * tau * tau * (10.0 - 15.0 * tau + 6.0 * tau * tau);
}

struct Mover {
    Trial& tr;
    Transformer& tf;
    Vec2 carry{};      // sub-count remainder of the hand movement
    Vec2 out_pos{};    // output-space position relative to start
    int64_t t_us;
    double poll_us;
    Rng& rng;

    void move(Vec2 hand, double duration_ms) {
        const int64_t t0 = t_us;
        const double dur_us = std::max(duration_ms, 5.0) * 1000.0;
        double done = 0.0;
        int64_t t = t0;
        while (true) {
            t += int64_t(poll_us * (1.0 + rng.uniform(-0.05, 0.05)));
            const double tau = double(t - t0) / dur_us;
            const double s = min_jerk(tau);
            const Vec2 want = hand * (s - done) + carry;
            done = s;
            const int32_t dx = int32_t(std::trunc(want.x));
            const int32_t dy = int32_t(std::trunc(want.y));
            carry = {want.x - dx, want.y - dy};
            if (dx != 0 || dy != 0) {
                MotionSample ms;
                ms.t_us = t;
                ms.in_dx = dx;
                ms.in_dy = dy;
                const Vec2 o = tf.apply(Vec2{double(dx), double(dy)}, t);
                ms.out_dx = o.x;
                ms.out_dy = o.y;
                out_pos += o;
                tr.motion.push_back(ms);
            }
            if (tau >= 1.0) break;
        }
        t_us = t;
    }
};

}  // namespace

Trial SimUser::run_trial(const TargetSpec& spec, const GameMapping& map, const Transform& active, double start_yaw,
                         double start_pitch, int64_t t_appear_us) {
    Trial tr;
    tr.spec = spec;
    const double k = map.deg_per_count();
    const double th = spec.dir_deg * kDegToRad;
    tr.target_counts = Vec2{spec.dist_deg * std::cos(th), -spec.dist_deg * std::sin(th)} / k;
    tr.start_yaw_deg = start_yaw;
    tr.start_pitch_deg = start_pitch;
    tr.t_appear_us = t_appear_us;

    Transformer tf(active);
    Mover mv{tr, tf, {}, {}, t_appear_us, 1e6 / p_.poll_hz, rng_};
    const double f = p_.speed_factor;
    const double deadline = double(t_appear_us) + p_.timeout_ms * 1000.0;

    // Leftover movement from the previous target (round-based tests): small, random direction, decaying.
    if (p_.settle_deg > 0.0) {
        const double a = rng_.uniform(0.0, 2.0 * kPi);
        const double d = p_.settle_deg * rng_.uniform(0.3, 1.0) / k;
        mv.move(Vec2{std::cos(a) * d, std::sin(a) * d}, p_.settle_ms);
    }
    // Reaction (the settling overlaps it).
    mv.t_us = std::max(mv.t_us, t_appear_us + int64_t(std::max(120.0, rng_.normal(p_.reaction_ms, p_.reaction_sd_ms)) * f * 1000.0));

    // Primary flick: planned for the baseline mapping, distorted by the motor map, plus noise.
    const double g = p_.primary_gain * std::exp(rng_.normal(0.0, p_.radial_sd));
    const double a = rng_.normal(0.0, p_.angle_sd_deg) * kDegToRad;
    const Vec2 plan = rotate_screen_ccw((tr.target_counts - mv.out_pos) * g, a);
    Vec2 hand = mul(p_, plan);
    hand.x *= hand.x < 0 ? p_.left_gain : p_.right_gain;  // raw counts: +x right, +y toward the bottom of the screen
    hand.y *= hand.y > 0 ? p_.down_gain : p_.up_gain;
    const double bits = std::log2(spec.dist_deg / std::max(spec.radius_deg * 2.0, 0.1) + 1.0);
    mv.move(hand, (p_.mt_base_ms + p_.mt_per_bit_ms * bits) * f);

    auto on_target = [&]() { return length(tr.target_counts - mv.out_pos) * k <= spec.radius_deg; };
    bool missed = false;
    auto click = [&](double delay_ms) {
        mv.t_us += int64_t(delay_ms * f * 1000.0);
        ClickEvent c;
        c.t_us = mv.t_us;
        c.offset_deg = length(tr.target_counts - mv.out_pos) * k;
        c.hit = c.offset_deg <= spec.radius_deg;
        tr.clicks.push_back(c);
        if (!c.hit && p_.single_click) missed = true;
        return c.hit;
    };

    bool hit = false;
    const double err_deg = length(tr.target_counts - mv.out_pos) * k;
    if (err_deg <= 2.0 * spec.radius_deg && rng_.uniform() < p_.early_click_prob) hit = click(p_.click_delay_ms * 0.6);
    if (missed && double(tr.clicks.back().t_us) <= deadline) {
        tr.outcome = TrialOutcome::Miss;
        tr.t_end_us = tr.clicks.back().t_us;
        return tr;
    }
    for (int c = 0; c < 6 && !hit && double(mv.t_us) < deadline; ++c) {
        if (on_target()) {
            hit = click(p_.click_delay_ms);
            break;
        }
        // Visually guided correction in output space; the player does not know the transform.
        const Vec2 e = tr.target_counts - mv.out_pos;
        const Vec2 corr = rotate_screen_ccw(e * (1.0 + rng_.normal(0.0, p_.corr_noise)), rng_.normal(0.0, 4.0) * kDegToRad);
        mv.t_us += int64_t(rng_.uniform(40.0, 90.0) * f * 1000.0);  // pause before correcting
        mv.move(corr, p_.corr_mt_ms * f * (0.7 + 0.3 * std::min(1.0, length(e) * k / 5.0)));
    }
    if (!hit && on_target() && double(mv.t_us) < deadline) hit = click(p_.click_delay_ms);

    if (hit && double(tr.clicks.back().t_us) <= deadline) {
        tr.outcome = TrialOutcome::Hit;
        tr.t_end_us = tr.clicks.back().t_us;
    } else {
        tr.outcome = TrialOutcome::Timeout;
        tr.t_end_us = int64_t(deadline);
        while (!tr.motion.empty() && tr.motion.back().t_us > tr.t_end_us) tr.motion.pop_back();
        while (!tr.clicks.empty() && tr.clicks.back().t_us > tr.t_end_us) tr.clicks.pop_back();
        for (auto& c : tr.clicks) c.hit = false;
    }
    return tr;
}

Vec2 SimUser::track_step(double now_s, double dt_s, double ty, double tp, double tvy, double tvp, double ay, double ap,
                        const GameMapping& map) {
    seen_.push_back({now_s, ty, tp, tvy, tvp, ay, ap});
    const double lag = p_.track_lag_ms / 1000.0;
    while (seen_.size() > 2 && seen_[1].t <= now_s - lag) seen_.erase(seen_.begin());
    const Seen& v = seen_.front();
    // Desired view velocity: the target's velocity as seen a reaction time ago, plus a correction of
    // the error seen then. Then the player's directional distortion, in raw count space.
    const double want_y = v.tvy + p_.track_correction * (v.ty - v.ay);
    const double want_p = v.tvp + p_.track_correction * (v.tp - v.ap);
    const double k = map.deg_per_count();
    Vec2 plan{want_y * dt_s / k, -want_p * dt_s / k};
    plan = plan * (1.0 + rng_.normal(0.0, p_.track_noise));
    Vec2 hand = mul(p_, plan);
    hand.x *= hand.x < 0 ? p_.left_gain : p_.right_gain;
    hand.y *= hand.y > 0 ? p_.down_gain : p_.up_gain;
    hand += track_carry_;
    const Vec2 out{std::trunc(hand.x), std::trunc(hand.y)};
    track_carry_ = hand - out;
    return out;
}

}  // namespace fit::sim
