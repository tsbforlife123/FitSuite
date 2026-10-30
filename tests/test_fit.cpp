#include <cmath>

#include "doctest.h"
#include "fit/mouse_fit.hpp"
#include "fit/mouse_schedule.hpp"
#include "sim_user.hpp"

using namespace fit;

namespace {

Session simulate_calibration(const sim::SimUserParams& p, uint64_t seed, int blocks = 4) {
    CalibrationPlanOptions o;
    o.blocks = blocks;
    o.seed = seed;
    const Plan plan = make_calibration_plan(o);
    Session s;
    s.id = "cal";
    s.kind = SessionKind::Calibration;
    s.conditions.mapping.sensitivity = 0.4;
    s.seed = seed;
    sim::SimUser u(p, seed * 7919 + 1);
    int64_t t = 0;
    for (const auto& b : plan.blocks)
        for (const auto& spec : b) {
            s.trials.push_back(u.run_trial(spec, s.conditions.mapping, {}, 0, 0, t));
            t = s.trials.back().t_end_us + 800'000;
        }
    s.complete = true;
    return s;
}

}  // namespace

TEST_CASE("fit recovers a vertical undershoot and proposes the opposite correction") {
    sim::SimUserParams p;
    p.m11 = 0.92;  // vertical flicks 8% short relative to horizontal
    const Session s = simulate_calibration(p, 21, 6);
    const FitResult r = fit_profiles(s);
    REQUIRE(r.ok);
    CHECK(r.gain_test.significant);
    CHECK(r.directional_recommended);
    // Relative to the average, up/down need ~+4% and left/right ~-4% (mean gain stays 1).
    const double g_up = r.directional.gain(90 * kDegToRad);
    const double g_right = r.directional.gain(0.0);
    CHECK(g_up > 1.02);
    CHECK(g_right < 0.985);
    CHECK(g_up / g_right == doctest::Approx(1.0 / 0.92).epsilon(0.04));
    // Raw Accel's Y/X ratio can represent this distortion.
    CHECK(r.raw_accel_recommended);
    CHECK(r.raw_accel.yx_ratio == doctest::Approx(1.0 / 0.92).epsilon(0.04));
    CHECK(r.max_radial_bias_after_dir_pct < r.max_radial_bias_before_pct);
    CHECK(r.max_radial_bias_after_ra_pct < r.max_radial_bias_before_pct);
}

TEST_CASE("fit recovers a constant rotation (sensor angle)") {
    sim::SimUserParams p;
    const double a = 3.0 * kDegToRad;  // hand movements rotated 3° counter-clockwise on screen
    // Screen-CCW rotation in raw coordinates.
    p.m00 = std::cos(a);
    p.m01 = std::sin(a);
    p.m10 = -std::sin(a);
    p.m11 = std::cos(a);
    const FitResult r = fit_profiles(simulate_calibration(p, 5, 6));
    REQUIRE(r.ok);
    CHECK(r.angle_test.significant);
    // The correction rotates back by ~3° (bounded at 3° by default).
    for (double th : {0.0, 1.0, 2.0, -2.5})
        CHECK(r.directional.angle(th) * kRadToDeg == doctest::Approx(-3.0).epsilon(0.25));
}

TEST_CASE("no false correction on an unbiased (isotropic) player") {
    int recommended = 0;
    const int sims = 120;
    for (int i = 0; i < sims; ++i) {
        sim::SimUserParams p;  // identity motor map, realistic noise
        const FitResult r = fit_profiles(simulate_calibration(p, 1000 + uint64_t(i), 4));
        REQUIRE(r.ok);
        if (r.directional_recommended || r.raw_accel_recommended) ++recommended;
    }
    // Three tests at alpha = 0.01 each: well under 10% in practice.
    MESSAGE("unbiased player: correction proposed in " << recommended << " of " << sims << " calibrations");
    CHECK(double(recommended) / sims <= 0.10);
}

TEST_CASE("validation data can never be used for fitting") {
    Session s = simulate_calibration({}, 3, 4);
    s.kind = SessionKind::Validation;
    const FitResult r = fit_profiles(s);
    CHECK_FALSE(r.ok);
    CHECK(r.message.find("calibration") != std::string::npos);

    Session c = simulate_calibration({}, 3, 4);
    c.trials[5].spec.condition = Condition::Candidate;
    CHECK_FALSE(fit_profiles(c).ok);
}

TEST_CASE("too little data gives a clear message instead of a profile") {
    const Session s = simulate_calibration({}, 9, 1);  // 24 flicks
    const FitResult r = fit_profiles(s);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.data_problems.empty());
    CHECK(r.directional.is_identity());
}

TEST_CASE("fitted profiles always respect the bounds") {
    sim::SimUserParams p;
    p.m11 = 0.70;  // extreme distortion: the correction must be capped, not followed
    FitOptions o;
    o.bounds = {0.05, 2.0};
    const FitResult r = fit_profiles(simulate_calibration(p, 13, 4), o);
    REQUIRE(r.ok);
    CHECK(r.directional.max_gain_deviation() <= 0.05 + 1e-9);
    CHECK(r.directional.max_angle_deg() <= 2.0 + 1e-9);
    CHECK(r.directional_bounds_scaled);
    CHECK(r.raw_accel.yx_ratio <= 1.05 + 1e-9);
    // The overall speed has its own limit; each direction is bounded relative to it.
    const double overall = 1.0 + r.overall_pct / 100.0;
    CHECK(std::fabs(overall - 1.0) <= 0.08 + 1e-9);
    for (int i = 0; i < 360; ++i) {  // per-direction bound holds for the Raw Accel candidate too
        const double th = (i + 0.5) * kDegToRad;
        CHECK(std::fabs(r.raw_accel.gain(th) / overall - 1.0) <= 0.05 + 1e-6);
        CHECK(std::fabs(r.raw_accel.angle(th)) * kRadToDeg <= 2.0 + 1e-6);
    }
}

TEST_CASE("a downward-only undershoot gets a downward-only correction") {
    // The example from the brief: flicks toward the bottom of the screen land 12% short, everything
    // else is fine. Separate up/down multipliers let Raw Accel boost downward movement only.
    sim::SimUserParams p;
    p.down_gain = 0.88;
    const FitResult r = fit_profiles(simulate_calibration(p, 31, 6));
    REQUIRE(r.ok);
    CHECK(r.raw_accel_recommended);
    const double down = r.raw_accel.gain(-90 * kDegToRad), up = r.raw_accel.gain(90 * kDegToRad);
    const double right = r.raw_accel.gain(0.0), left = r.raw_accel.gain(180 * kDegToRad);
    MESSAGE("down " << down << " up " << up << " left " << left << " right " << right << " (yx " << r.raw_accel.yx_ratio
                    << ", ud " << r.raw_accel.ud_ratio << ", lr " << r.raw_accel.lr_ratio << ")");
    CHECK(down / up > 1.06);                     // downward boosted relative to upward (capped at 8%)
    CHECK(down / right > 1.04);
    CHECK(up / right == doctest::Approx(1.0).epsilon(0.03));
    CHECK(left / right == doctest::Approx(1.0).epsilon(0.03));
    CHECK(r.max_radial_bias_after_ra_pct < r.max_radial_bias_before_pct);
    // The smooth profile sees it too.
    CHECK(r.directional.gain(-90 * kDegToRad) / r.directional.gain(90 * kDegToRad) > 1.04);
}

TEST_CASE("the speed curve: built so a typical flick keeps its distance, within its bounds") {
    const SpeedCurve c = make_speed_curve(0.08, 3.0, 1.5, 6.0);
    REQUIRE(c.points.size() >= 10);
    for (size_t i = 1; i < c.points.size(); ++i) CHECK(c.points[i].first > c.points[i - 1].first);
    CHECK(c.max_deviation() <= kSpeedCurveMaxDeviation + 1e-6);
    CHECK(c.points.back().second == c.points[c.points.size() - 2].second);  // flat end: Raw Accel extends the last segment
    CHECK(speed_curve_flick_gain(c, 3.0) == doctest::Approx(1.0).epsilon(0.003));
    CHECK(speed_curve_flick_gain(c, 6.0) > 1.03);   // fast flicks: more sensitivity
    CHECK(speed_curve_flick_gain(c, 1.5) < 0.97);   // slow flicks: less
    const SpeedCurve d = make_speed_curve(-0.08, 3.0, 1.5, 6.0);
    CHECK(speed_curve_flick_gain(d, 6.0) < 0.97);
}

TEST_CASE("far targets landing short gets a speed curve; an even player does not") {
    sim::SimUserParams p;
    p.dist_gain_slope = -0.10;  // 26° flicks land about 8% shorter than 12° ones, 6° about 7% longer
    int found = 0;
    for (uint64_t i = 0; i < 8; ++i) {
        const FitResult r = fit_profiles(simulate_calibration(p, 500 + i, 6));
        REQUIRE(r.ok);
        if (i == 0)
            MESSAGE("speed curve: p = " << r.speed_test.p_value << ", k = " << r.speed_exponent << ", slow "
                                        << r.speed_slow_pct << "%, fast " << r.speed_fast_pct << "%, level bias "
                                        << r.speed_level_bias_before_pct << "% -> " << r.speed_level_bias_after_pct << "%");
        if (r.speed_recommended && r.raw_accel_recommended && r.speed_exponent > 0.0 &&
            speed_curve_flick_gain(r.raw_accel.speed, r.speed_fast_cpms) > 1.0 &&
            r.speed_level_bias_after_pct < r.speed_level_bias_before_pct)
            ++found;
    }
    MESSAGE("distance-dependent landing found in " << found << " of 8 calibrations");
    CHECK(found >= 7);

    int invented = 0;
    for (uint64_t i = 0; i < 60; ++i) {
        const FitResult r = fit_profiles(simulate_calibration({}, 2000 + i, 4));
        REQUIRE(r.ok);
        if (r.speed_recommended) ++invented;
    }
    MESSAGE("even player: speed curve proposed in " << invented << " of 60 calibrations");
    CHECK(invented <= 4);
}

TEST_CASE("overall speed: first movements short everywhere give a faster overall speed, capped, on its own test") {
    sim::SimUserParams p;
    p.primary_gain = 0.95;  // 5% short in every direction, no direction differences
    const Session s = simulate_calibration(p, 99, 4);
    const FitResult r = fit_profiles(s);
    REQUIRE(r.ok);
    CHECK(r.overall_recommended);
    CHECK(r.fix_recommended());
    CHECK(r.overall_pct > 2.0);
    CHECK(r.overall_pct <= 8.0 + 1e-9);
    CHECK(r.raw_accel.sens > 1.02);

    FitOptions no;
    no.overall_speed = false;
    const FitResult r2 = fit_profiles(s, no);
    CHECK_FALSE(r2.overall_recommended);
    CHECK(r2.raw_accel.sens == doctest::Approx(r.raw_accel.sens / (1.0 + r.overall_pct / 100.0)).epsilon(1e-6));
}
