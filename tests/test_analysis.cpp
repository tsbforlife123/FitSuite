#include <algorithm>
#include <cmath>

#include "doctest.h"
#include "fit/mouse_analysis.hpp"
#include "sim_user.hpp"

using namespace fit;

namespace {

double mj(double tau) {
    tau = std::clamp(tau, 0.0, 1.0);
    return tau * tau * tau * (10 - 15 * tau + 6 * tau * tau);
}

// Builds a trial: rest for `rt_ms`, a minimum-jerk primary to `primary` (counts), a pause, a
// correction to the exact target, then a click.
Trial make_trial(Vec2 target, Vec2 primary, double rt_ms, double mt_ms, double pause_ms, double corr_ms,
                 bool click_early = false, double poll_ms = 1.0) {
    Trial tr;
    tr.spec.dir_deg = screen_angle(target) * kRadToDeg;
    tr.spec.dist_deg = 10;
    tr.spec.radius_deg = 0.5;
    tr.target_counts = target;
    tr.t_appear_us = 1'000'000;
    tr.outcome = TrialOutcome::Hit;
    int64_t t = tr.t_appear_us + int64_t(rt_ms * 1000);
    Vec2 carry{};
    auto move = [&](Vec2 v, double dur) {
        const int64_t t0 = t;
        double done = 0;
        while (true) {
            t += int64_t(poll_ms * 1000);
            const double tau = double(t - t0) / (dur * 1000.0);
            const double s = mj(tau);
            Vec2 w = v * (s - done) + carry;
            done = s;
            const int dx = int(std::trunc(w.x)), dy = int(std::trunc(w.y));
            carry = {w.x - dx, w.y - dy};
            if (dx || dy) tr.motion.push_back({t, dx, dy, double(dx), double(dy)});
            if (tau >= 1) break;
        }
    };
    move(primary, mt_ms);
    if (click_early) tr.clicks.push_back({t + 10'000, false, 2.0});
    t += int64_t(pause_ms * 1000);
    Vec2 pos{};
    for (const auto& m : tr.motion) pos += Vec2{m.out_dx, m.out_dy};
    move(target - pos, corr_ms);
    t += 60'000;
    tr.clicks.push_back({t, true, 0.1});
    tr.t_end_us = t;
    return tr;
}

GameMapping mapping() {
    GameMapping m;
    m.sensitivity = 0.4;  // 0.028 deg/count
    return m;
}

}  // namespace

TEST_CASE("first-flick undershoot is measured before the correction") {
    const Vec2 target{400, 0};
    const Trial tr = make_trial(target, {368, 0}, 220, 180, 120, 150);  // 92% primary
    const auto m = analyze_trial(tr, mapping());
    REQUIRE(m.valid);
    CHECK(m.radial_ratio == doctest::Approx(0.92).epsilon(0.02));
    CHECK(m.radial_error_pct == doctest::Approx(-8.0).epsilon(0.25));
    CHECK(m.flick_class == FlickClass::Undershoot);
    CHECK(m.reaction_ms == doctest::Approx(220).epsilon(0.12));
    CHECK(m.hit);
    CHECK(m.correction_ms > 150);
    CHECK(m.acquisition_ms > m.reaction_ms + m.primary_ms);
    // The final click is on target, but the primary error was still recorded.
    CHECK(std::fabs(m.endpoint_error_deg - 32 * 0.028) < 0.15);
}

TEST_CASE("overshoot and angular error sign conventions") {
    // Target up (screen), primary 110% and rotated 4° counter-clockwise on screen (toward the left).
    const Vec2 target{0, -500};
    const double a = 4.0 * kDegToRad;
    const Vec2 primary = rotate_screen_ccw(target * 1.10, a);
    const auto m = analyze_trial(make_trial(target, primary, 250, 200, 100, 150), mapping());
    REQUIRE(m.valid);
    CHECK(m.flick_class == FlickClass::Overshoot);
    CHECK(m.radial_error_pct == doctest::Approx(10.0 * std::cos(a) + (std::cos(a) - 1) * 100).epsilon(0.15));
    CHECK(m.angle_error_deg == doctest::Approx(4.0).epsilon(0.15));
    CHECK(primary.x < 0);  // CCW from "up" is toward the left
}

TEST_CASE("a click during the flick ends the primary at the click (flick shot)") {
    Trial tr = make_trial({300, 300}, {300, 300}, 200, 160, 0, 30);
    tr.clicks.clear();
    // Click 100 ms into the 160 ms flick and count it as the hit.
    const int64_t tc = tr.t_appear_us + 300'000;
    tr.clicks.push_back({tc, true, 0.5});
    tr.t_end_us = tc;
    tr.motion.erase(std::remove_if(tr.motion.begin(), tr.motion.end(), [&](const MotionSample& s) { return s.t_us > tc; }),
                    tr.motion.end());
    const auto m = analyze_trial(tr, mapping());
    REQUIRE(m.valid);
    CHECK(m.end_rule == PrimaryEndRule::Click);
    CHECK(m.first_click_hit);
    CHECK(m.correction_ms == doctest::Approx(0.0).epsilon(0.01));
}

TEST_CASE("invalid trials are flagged, not silently used") {
    SUBCASE("interrupted") {
        Trial tr = make_trial({300, 0}, {290, 0}, 200, 160, 50, 100);
        tr.outcome = TrialOutcome::Interrupted;
        CHECK_FALSE(analyze_trial(tr, mapping()).valid);
    }
    SUBCASE("anticipation") {
        const auto m = analyze_trial(make_trial({300, 0}, {290, 0}, 40, 160, 50, 100), mapping());
        CHECK_FALSE(m.valid);
        CHECK(m.invalid_reason.find("before the target") != std::string::npos);
    }
    SUBCASE("no movement") {
        Trial tr;
        tr.target_counts = {300, 0};
        tr.t_appear_us = 0;
        tr.t_end_us = 3'000'000;
        tr.outcome = TrialOutcome::Timeout;
        CHECK_FALSE(analyze_trial(tr, mapping()).valid);
    }
    SUBCASE("gap in the input stream during the flick") {
        Trial tr = make_trial({600, 0}, {580, 0}, 200, 300, 50, 100);
        // Remove 80 ms of reports from the middle of the flick.
        const int64_t a = tr.t_appear_us + 300'000, b = a + 80'000;
        tr.motion.erase(std::remove_if(tr.motion.begin(), tr.motion.end(),
                                       [&](const MotionSample& s) { return s.t_us > a && s.t_us < b; }),
                        tr.motion.end());
        CHECK_FALSE(analyze_trial(tr, mapping()).valid);
    }
}

TEST_CASE("segmentation is stable at low polling rates") {
    const Vec2 target{500, -200};
    const auto m1000 = analyze_trial(make_trial(target, target * 0.9, 220, 200, 120, 150, false, 1.0), mapping());
    const auto m125 = analyze_trial(make_trial(target, target * 0.9, 220, 200, 120, 150, false, 8.0), mapping());
    REQUIRE(m1000.valid);
    REQUIRE(m125.valid);
    CHECK(m125.radial_ratio == doctest::Approx(m1000.radial_ratio).epsilon(0.03));
    CHECK(m125.reaction_ms == doctest::Approx(m1000.reaction_ms).epsilon(0.1));
}

TEST_CASE("synthetic player: measured bias matches the simulated motor distortion") {
    sim::SimUserParams p;
    p.radial_sd = 0.0;
    p.angle_sd_deg = 0.0;
    p.m11 = 0.9;  // vertical movements come out 10% short
    sim::SimUser u(p, 1);
    const GameMapping map = mapping();
    TargetSpec right, up;
    right.dir_deg = 0;
    right.dist_deg = 14;
    up.dir_deg = 90;
    up.dist_deg = 14;
    const auto mr = analyze_trial(u.run_trial(right, map, {}, 0, 0, 0), map);
    const auto mu = analyze_trial(u.run_trial(up, map, {}, 0, 0, 0), map);
    REQUIRE(mr.valid);
    REQUIRE(mu.valid);
    CHECK(mr.radial_ratio == doctest::Approx(0.93).epsilon(0.02));
    CHECK(mu.radial_ratio == doctest::Approx(0.93 * 0.9).epsilon(0.02));
}

TEST_CASE("polling rate estimate") {
    sim::SimUserParams p;
    p.poll_hz = 1000;
    sim::SimUser u(p, 3);
    std::vector<Trial> ts;
    TargetSpec s;
    s.dist_deg = 20;
    for (int i = 0; i < 5; ++i) ts.push_back(u.run_trial(s, mapping(), {}, 0, 0, 0));
    CHECK(estimate_polling_hz(ts) == doctest::Approx(1000).epsilon(0.08));
}
