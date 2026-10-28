#include <cmath>
#include <vector>

#include "doctest.h"
#include "fit/mouse_check.hpp"
#include "fit/mouse_fit.hpp"
#include "fit/mouse_schedule.hpp"
#include "sim_user.hpp"

using namespace fit;

namespace {

Session simulate_calibration(const sim::SimUserParams& p, uint64_t seed, int blocks) {
    CalibrationPlanOptions o;
    o.blocks = blocks;
    o.seed = seed;
    const Plan plan = make_calibration_plan(o);
    Session s;
    s.id = "cal";
    s.kind = SessionKind::Calibration;
    s.conditions.mapping.sensitivity = 0.4;
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

// A quick check exactly as the app records it: the fix is applied to every flick.
Session simulate_check(const sim::SimUserParams& p, const Transform& fix, uint64_t seed, int blocks = 3) {
    CheckPlanOptions o;
    o.blocks = blocks;
    o.seed = seed;
    const Plan plan = make_check_plan(o);
    Session s;
    s.id = "chk";
    s.kind = SessionKind::Check;
    s.conditions.mapping.sensitivity = 0.4;
    // Played as the app does it: one click per target, the next target right away.
    sim::SimUserParams q = p;
    q.single_click = true;
    q.settle_deg = 0.4;
    q.timeout_ms = o.target_timeout_ms;
    sim::SimUser u(q, seed * 104729 + 7);
    int64_t t = 0;
    for (const auto& b : plan.blocks)
        for (const auto& spec : b) {
            s.trials.push_back(u.run_trial(spec, s.conditions.mapping, fix, 0, 0, t));
            s.trials.back().timeout_ms = o.target_timeout_ms;
            t = s.trials.back().t_end_us;
        }
    s.complete = true;
    return s;
}

Transform fix_from(const FitResult& r) {
    Transform t;
    t.kind = TransformKind::RawAccel;
    t.raw_accel = r.raw_accel;
    return t;
}

}  // namespace

TEST_CASE("the check plan has 48 targets in 8 directions, all with the fix in use") {
    const Plan p = make_check_plan({});
    CHECK(p.kind == SessionKind::Check);
    CHECK(p.scored_trials() == 48);
    for (const auto& b : p.blocks)
        for (const auto& s : b) CHECK(s.condition == Condition::Candidate);
    CheckPlanOptions everywhere;
    everywhere.condition = Condition::Baseline;  // Raw Accel already applies the fix
    for (const auto& b : make_check_plan(everywhere).blocks)
        for (const auto& s : b) CHECK(s.condition == Condition::Baseline);
}

TEST_CASE("an unchanged player is told the fix still fits") {
    // A player with an 8% vertical undershoot, whose fix comes from a normal 4-block measurement.
    sim::SimUserParams p;
    p.m11 = 0.92;
    const Session cal_session = simulate_calibration(p, 77, 4);
    const FitResult cal = fit_profiles(cal_session);
    REQUIRE(cal.ok);
    REQUIRE(cal.raw_accel_recommended);
    const Transform fix = fix_from(cal);
    const DirectionalProfile expected = expected_leftover(cal_session, fix);
    int changed = 0, not_enough = 0;
    const int sims = 150;
    for (int i = 0; i < sims; ++i) {
        const CheckResult r = check_fix(simulate_check(p, fix, 4000 + uint64_t(i)), expected);
        if (r.verdict == CheckVerdict::Changed) ++changed;
        if (r.verdict == CheckVerdict::NotEnoughData) ++not_enough;
    }
    MESSAGE("unchanged player: \"changed\" in " << changed << " of " << sims << " checks, not enough data in " << not_enough);
    CHECK(double(changed) / sims <= 0.08);
    CHECK(double(not_enough) / sims <= 0.05);
}

TEST_CASE("a fix capped at its bounds is not mistaken for a change in aim") {
    // A precise player with a 14% vertical undershoot: the fix can only take 8% of it, so flicks with
    // the fix still land short vertically. The check has to compare with that expected leftover.
    sim::SimUserParams p;
    p.m11 = 0.86;
    p.primary_gain = 0.99;
    p.radial_sd = 0.045;
    p.angle_sd_deg = 1.5;
    int changed = 0, changed_naive = 0;
    const int sims = 120;
    for (uint64_t k = 0; k < 4; ++k) {
        const Session cal = simulate_calibration(p, 178 + k, 4);
        const FitResult fit = fit_profiles(cal);
        REQUIRE(fit.ok);
        const Transform fix = fix_from(fit);
        const DirectionalProfile expected = expected_leftover(cal, fix);
        CHECK_FALSE(expected.is_identity());
        for (int i = 0; i < sims / 4; ++i) {
            const Session chk = simulate_check(p, fix, 5000 + k * 100 + uint64_t(i));
            if (check_fix(chk, expected).verdict == CheckVerdict::Changed) ++changed;
            if (check_fix(chk).verdict == CheckVerdict::Changed) ++changed_naive;
        }
    }
    MESSAGE("capped fix, unchanged player: \"changed\" in " << changed << " of " << sims << " (" << changed_naive
                                                              << " when compared with zero instead)");
    CHECK(double(changed) / sims <= 0.10);
    CHECK(changed_naive > 3 * changed);
}

TEST_CASE("an unbiased player without a fix is not sent to re-measure") {
    int changed = 0;
    const int sims = 150;
    for (int i = 0; i < sims; ++i)
        if (check_fix(simulate_check({}, Transform{}, 9000 + uint64_t(i))).verdict == CheckVerdict::Changed) ++changed;
    MESSAGE("unbiased player, no fix: \"changed\" in " << changed << " of " << sims);
    CHECK(double(changed) / sims <= 0.08);
}

TEST_CASE("a clear change in aim is noticed") {
    // Fixes from four separate measurements of a player with an 8% vertical undershoot.
    sim::SimUserParams before;
    before.m11 = 0.92;
    std::vector<Transform> fixes;
    std::vector<DirectionalProfile> expected;
    for (uint64_t k = 0; k < 4; ++k) {
        const Session cs = simulate_calibration(before, 78 + k, 4);
        const FitResult cal = fit_profiles(cs);
        REQUIRE(cal.ok);
        fixes.push_back(fix_from(cal));
        expected.push_back(expected_leftover(cs, fixes.back()));
    }
    // Weeks later: the vertical undershoot is gone, and downward flicks now land 12% short instead.
    sim::SimUserParams after;
    after.down_gain = 0.88;
    int changed = 0;
    const int sims = 120;
    for (int i = 0; i < sims; ++i)
        if (check_fix(simulate_check(after, fixes[size_t(i) % fixes.size()], 6000 + uint64_t(i)), expected[size_t(i) % fixes.size()])
                .verdict == CheckVerdict::Changed)
            ++changed;
    MESSAGE("changed player: noticed in " << changed << " of " << sims << " quick checks");
    CHECK(double(changed) / sims >= 0.6);
    // A larger change (the vertical undershoot doubled) is almost always noticed.
    sim::SimUserParams worse;
    worse.m11 = 0.92 * 0.92;
    int noticed = 0;
    for (int i = 0; i < 60; ++i)
        if (check_fix(simulate_check(worse, fixes[size_t(i) % fixes.size()], 7000 + uint64_t(i)), expected[size_t(i) % fixes.size()])
                .verdict == CheckVerdict::Changed)
            ++noticed;
    MESSAGE("doubled undershoot: noticed in " << noticed << " of 60");
    CHECK(noticed >= 48);
    // The explanation names what is off.
    for (int i = 0; i < 10; ++i) {
        const CheckResult one = check_fix(simulate_check(after, fixes[0], 6500 + uint64_t(i)), expected[0]);
        if (one.verdict != CheckVerdict::Changed) continue;
        CHECK(one.explanation.find("Measure again") != std::string::npos);
        break;
    }
}

TEST_CASE("too few flicks or the wrong session give no verdict") {
    const CheckResult few = check_fix(simulate_check({}, Transform{}, 5, 1));  // 16 flicks
    CHECK(few.verdict == CheckVerdict::NotEnoughData);
    CHECK_FALSE(few.explanation.empty());
    Session cal = simulate_calibration({}, 3, 2);
    const CheckResult wrong = check_fix(cal);
    CHECK(wrong.verdict == CheckVerdict::NotEnoughData);
}
