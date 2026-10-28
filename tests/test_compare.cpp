#include <cmath>

#include "doctest.h"
#include "fit/mouse_compare.hpp"
#include "fit/mouse_schedule.hpp"
#include "sim_user.hpp"

using namespace fit;

namespace {

// Simulates a full validation session. `block_sd` adds block-to-block drift (fatigue, focus) and
// `trend` a steady practice effect, both of which a naive analysis could mistake for an effect.
Session simulate_validation(const sim::SimUserParams& p, const Transform& candidate, uint64_t seed, double block_sd = 0.03,
                            double trend = -0.004) {
    ValidationPlanOptions o;
    o.seed = seed;
    const Plan plan = make_validation_plan(o);
    Session s;
    s.id = "val";
    s.kind = SessionKind::Validation;
    s.seed = seed;
    s.conditions.mapping.sensitivity = 0.4;
    // As the app plays it: one click per target (a miss makes it disappear), the next target right
    // away (the hand may still be settling), a 2.2 s timer per target.
    sim::SimUserParams q = p;
    q.single_click = true;
    if (q.settle_deg == 0.0) q.settle_deg = 0.4;
    q.timeout_ms = o.target_timeout_ms;
    sim::SimUser u(q, seed * 104729 + 17);
    Rng drift(seed ^ 0xD1F7);
    int64_t t = 0;
    for (size_t b = 0; b < plan.blocks.size(); ++b) {
        u.params().speed_factor = std::exp(drift.normal(0.0, block_sd) + trend * double(b));
        const Transform& tf = plan.block_condition[b] == Condition::Candidate ? candidate : Transform{};
        for (const auto& spec : plan.blocks[b]) {
            s.trials.push_back(u.run_trial(spec, s.conditions.mapping, tf, 0, 0, t));
            s.trials.back().timeout_ms = o.target_timeout_ms;
            t = s.trials.back().t_end_us;
        }
    }
    s.complete = true;
    return s;
}

Transform yx_candidate(double ratio) {
    Transform t;
    t.kind = TransformKind::RawAccel;
    t.raw_accel.yx_ratio = ratio;
    return t;
}

}  // namespace

TEST_CASE("no false improvement when baseline and candidate are identical") {
    const int sims = 200;
    int improved = 0, worse = 0;
    for (int i = 0; i < sims; ++i) {
        CompareOptions o;
        o.bootstrap_reps = 600;
        const auto r = compare_session(simulate_validation({}, Transform{}, 500 + uint64_t(i)), o);
        REQUIRE(r.ok);
        if (r.verdict == Verdict::Improved) ++improved;
        if (r.verdict == Verdict::Worse) ++worse;
    }
    MESSAGE("identical candidate: improved " << improved << ", worse " << worse << " of " << sims << " comparisons");
    CHECK(double(improved) / sims <= 0.05);
    CHECK(double(worse) / sims <= 0.10);
}

TEST_CASE("no false improvement on noisy data with strong drift") {
    const int sims = 120;
    int improved = 0;
    for (int i = 0; i < sims; ++i) {
        sim::SimUserParams p;
        p.radial_sd = 0.12;
        p.angle_sd_deg = 4.0;
        CompareOptions o;
        o.bootstrap_reps = 600;
        const auto r = compare_session(simulate_validation(p, Transform{}, 9000 + uint64_t(i), 0.08, -0.01), o);
        if (r.verdict == Verdict::Improved) ++improved;
    }
    MESSAGE("noisy data with drift: improved " << improved << " of " << sims << " comparisons");
    CHECK(double(improved) / sims <= 0.05);
}

TEST_CASE("a real correction is detected") {
    // Player with 10% vertical undershoot; candidate corrects it exactly.
    sim::SimUserParams p;
    p.m11 = 0.90;
    p.corr_mt_ms = 180;
    int improved = 0;
    const int sims = 30;
    for (int i = 0; i < sims; ++i) {
        CompareOptions o;
        o.bootstrap_reps = 600;
        const auto r = compare_session(simulate_validation(p, yx_candidate(1.0 / 0.90), 70 + uint64_t(i)), o);
        if (r.verdict == Verdict::Improved) ++improved;
        CHECK(r.verdict != Verdict::Worse);
    }
    MESSAGE("10% vertical undershoot, exact candidate: improved " << improved << " of " << sims << " comparisons (8 blocks)");
    CHECK(improved >= sims / 2);  // power depends on the effect size and session length
}

TEST_CASE("a harmful candidate is not recommended") {
    sim::SimUserParams p;  // unbiased player
    CompareOptions o;
    o.bootstrap_reps = 600;
    int recommended = 0;
    for (int i = 0; i < 20; ++i) {
        const auto r = compare_session(simulate_validation(p, yx_candidate(1.15), 300 + uint64_t(i)), o);
        if (r.recommend_candidate) ++recommended;
    }
    MESSAGE("15% too much vertical sensitivity: recommended in " << recommended << " of 20");
    CHECK(recommended == 0);
}

TEST_CASE("only validation sessions are compared; too few trials is reported") {
    Session s = simulate_validation({}, Transform{}, 1);
    s.kind = SessionKind::Calibration;
    CHECK_FALSE(compare_session(s).ok);

    Session few = simulate_validation({}, Transform{}, 2);
    few.trials.resize(30);
    const auto r = compare_session(few);
    CHECK(r.verdict == Verdict::InsufficientData);
    CHECK_FALSE(r.recommend_candidate);
}
