#include <cmath>
#include <set>

#include "doctest.h"
#include "fit/mouse_schedule.hpp"

using namespace fit;

TEST_CASE("calibration plan: every direction × distance once per block") {
    CalibrationPlanOptions o;
    const Plan p = make_calibration_plan(o);
    REQUIRE(p.blocks.size() == 4);
    for (const auto& b : p.blocks) {
        CHECK(b.size() == 24);
        std::set<std::pair<long, long>> seen;
        for (const auto& s : b) {
            CHECK(s.condition == Condition::Baseline);
            seen.insert({std::lround(s.dir_deg), std::lround(s.dist_deg * 10)});
        }
        CHECK(seen.size() == 24);
    }
}

TEST_CASE("validation targets are fresh: never a calibration direction/distance") {
    const Plan cal = make_calibration_plan({});
    for (uint64_t seed = 1; seed < 40; ++seed) {
        ValidationPlanOptions o;
        o.seed = seed;
        const Plan val = make_validation_plan(o);
        for (const auto& b : val.blocks)
            for (const auto& v : b) {
                // Calibration directions are multiples of 45°; validation is offset 22.5° ± 6°.
                const double off = std::fmod(v.dir_deg, 45.0);
                CHECK(std::min(off, 45.0 - off) >= 16.4);
                for (const auto& cb : cal.blocks)
                    for (const auto& c : cb) CHECK(std::fabs(c.dist_deg - v.dist_deg) / c.dist_deg > 0.05);
            }
    }
}

TEST_CASE("validation order is counterbalanced and hidden from labels") {
    std::set<std::string> orders;
    for (uint64_t seed = 1; seed < 30; ++seed) {
        ValidationPlanOptions o;
        o.seed = seed;
        const Plan p = make_validation_plan(o);
        REQUIRE(p.blocks.size() == 8);
        orders.insert(p.order_label);
        int a = 0, b = 0;
        for (size_t i = 0; i < p.blocks.size(); ++i) {
            const Condition c = p.block_condition[i];
            (c == Condition::Baseline ? a : b)++;
            for (const auto& s : p.blocks[i]) CHECK(s.condition == c);
        }
        CHECK(a == 4);
        CHECK(b == 4);
        // Adjacent pairs always contain one of each (needed for the pair-consistency check).
        for (size_t i = 0; i + 1 < p.blocks.size(); i += 2) CHECK(p.block_condition[i] != p.block_condition[i + 1]);
        for (const auto& s : p.warmup) CHECK(s.purpose == TrialPurpose::Warmup);
    }
    CHECK(orders.size() == 2);  // ABBABAAB and BAABABBA both occur
}

TEST_CASE("trial picker keeps the camera near the horizon and finishes every block") {
    for (uint64_t seed = 1; seed < 60; ++seed) {
        CalibrationPlanOptions o;
        o.seed = seed;
        const Plan p = make_calibration_plan(o);
        for (const auto& block : p.blocks) {
            TrialPicker picker(block, seed);
            double pitch = 0.0;
            int steps = 0, positioning = 0;
            while (!picker.done() && steps < 200) {
                const TargetSpec s = picker.next(pitch);
                pitch += s.dist_deg * std::sin(s.dir_deg * kDegToRad);  // perfect aim
                CHECK(std::fabs(pitch) <= 20.0);  // hard limit 16° plus at most a small positioning step
                if (s.purpose == TrialPurpose::Recenter) ++positioning;
                else picker.complete(s.index);
                ++steps;
            }
            CHECK(picker.done());
            CHECK(positioning <= 12);
        }
    }
}

TEST_CASE("an interrupted trial stays queued") {
    const Plan p = make_calibration_plan({});
    TrialPicker picker(p.blocks[0], 1);
    const TargetSpec s = picker.next(0.0);
    CHECK(picker.remaining() == 24);  // not completed yet
    picker.complete(s.index);
    CHECK(picker.remaining() == 23);
    picker.complete(s.index);  // completing twice is harmless
    CHECK(picker.remaining() == 23);
}
