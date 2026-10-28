#include <cmath>

#include "doctest.h"
#include "fit/motion.hpp"
#include "fit/rng.hpp"

using namespace fit;

namespace {

// Simulated observer: true 75%-correct threshold `th` (screen widths/s) for each configuration.
std::vector<MotionResponse> simulate(const MotionPlan& plan, const std::vector<double>& th, uint64_t seed) {
    Rng r(seed);
    std::vector<MotionResponse> out;
    const double slope = 0.35;
    const double q = 0.5 / (0.75 * 0.98);
    const double z75 = std::log(q / (1 - q));
    for (const auto& b : plan.blocks)
        for (const auto& s : b) {
            const double mu = std::log2(th[size_t(s.config)]) + z75 * slope;
            const double x = std::log2(s.speed_sw_s);
            const double p = 0.25 + 0.75 * 0.98 / (1 + std::exp((x - mu) / slope));
            MotionResponse m;
            m.spec = s;
            m.correct = r.uniform() < p;
            m.answer = m.correct ? s.orientation : (s.orientation + 1) % 4;
            out.push_back(m);
        }
    return out;
}

}  // namespace

TEST_CASE("motion plan alternates configurations and balances speeds") {
    MotionPlanOptions o;
    const MotionPlan p = make_motion_plan(2, o);
    REQUIRE(p.blocks.size() == 6);
    for (size_t i = 1; i < p.block_config.size(); ++i) CHECK(p.block_config[i] != p.block_config[i - 1]);
    for (const auto& b : p.blocks) CHECK(b.size() == o.speeds_sw_s.size() * size_t(o.reps_per_speed_per_block));
}

TEST_CASE("psychometric fit recovers the threshold") {
    MotionPlanOptions o;
    o.blocks_per_config = 8;
    o.reps_per_speed_per_block = 4;
    o.speeds_sw_s = {0.4, 0.6, 0.8, 1.0, 1.3, 1.7, 2.2};
    const MotionPlan p = make_motion_plan(1, o);
    const auto rs = simulate(p, {1.0}, 7);
    const auto f = fit_psychometric(rs, 0);
    REQUIRE(f.ok);
    CHECK(f.threshold_sw_s == doctest::Approx(1.0).epsilon(0.15));
    CHECK(f.threshold_ci.lo <= f.threshold_sw_s);
    CHECK(f.threshold_ci.hi >= f.threshold_sw_s);
}

TEST_CASE("motion comparison: real difference detected, identical setups not") {
    MotionPlanOptions o;
    o.blocks_per_config = 6;
    o.reps_per_speed_per_block = 4;
    o.speeds_sw_s = {0.4, 0.6, 0.8, 1.0, 1.3, 1.7, 2.2};
    const MotionPlan p = make_motion_plan(2, o);
    const auto better = compare_motion(simulate(p, {0.8, 1.4}, 3), 0, 1);
    CHECK(better.verdict == "better");
    int false_pos = 0;
    for (uint64_t s = 0; s < 40; ++s) {
        const auto same = compare_motion(simulate(p, {1.0, 1.0}, 100 + s), 0, 1, 200);
        if (same.verdict == "better" || same.verdict == "worse") ++false_pos;
    }
    CHECK(false_pos <= 4);
}

TEST_CASE("a monitor below its fastest refresh rate is noticed") {
    using namespace fit;
    const RefreshAdvice slow = refresh_advice(60.0, {60.0, 100.0, 120.0, 144.0});
    CHECK(slow.slower);
    CHECK(slow.max_hz == doctest::Approx(144.0));
    CHECK_FALSE(refresh_advice(144.0, {60.0, 120.0, 144.0}).slower);
    CHECK_FALSE(refresh_advice(59.0, {59.0, 60.0}).slower);       // 59 vs 60 Hz is not worth a warning
    CHECK(refresh_advice(165.0, {60.0, 144.0, 165.0, 170.0}).slower);  // 5 Hz is
    CHECK_FALSE(refresh_advice(0.0, {60.0, 144.0}).slower);       // current rate unknown
    CHECK_FALSE(refresh_advice(60.0, {}).slower);                 // no modes reported
    CHECK(refresh_dismiss_key("DEL4321", 143.9) == "DEL4321@144");
    CHECK(refresh_dismiss_key("DEL4321", 144.0) != refresh_dismiss_key("DEL4321", 240.0));
}
