#include <cmath>
#include <random>

#include "doctest.h"
#include "fit/live_tuning.hpp"

using namespace fit;

namespace {

constexpr double kDpc = 0.028;  // VALORANT 0.07° × sensitivity 0.4

// Feeds a smooth (minimum-jerk) movement from `from` to `to` over `ms` at 1000 reports a second.
struct Feeder {
    FlickDetector& d;
    int64_t t = 1'000'000;
    Vec2 pos{};
    Vec2 sent{};  // integer counts already sent

    void move_to(Vec2 to, double ms) {
        const Vec2 from = pos;
        const int n = int(ms);
        for (int i = 1; i <= n; ++i) {
            const double s = double(i) / double(n);
            const double k = 10 * s * s * s - 15 * s * s * s * s + 6 * s * s * s * s * s;
            pos = from + (to - from) * k;
            const int dx = int(std::lround(pos.x - sent.x)), dy = int(std::lround(pos.y - sent.y));
            t += 1000;
            if (dx == 0 && dy == 0) continue;
            sent += Vec2{double(dx), double(dy)};
            d.add({t, dx, dy, false});
        }
    }
    void wait(double ms) {
        t += int64_t(ms * 1000);
        d.tick(t);
    }
    void click() {
        t += 1000;
        d.add({t, 0, 0, true});
    }
    // One aimed flick: a first movement to `primary`, a correction to `target`, a stop, a click.
    void flick(Vec2 target, Vec2 primary) {
        const Vec2 o = pos;
        move_to(o + primary, 160);
        if (length(target - primary) > 0.5) move_to(o + target, 130);
        wait(150);
        click();
        wait(500);
    }
};

Vec2 dir_deg(double screen_deg, double counts) {
    return raw_unit_from_screen_angle(screen_deg * kDegToRad) * counts;
}

}  // namespace

TEST_CASE("live tuning finds aimed flicks and judges them by how they were finished") {
    FlickDetectorOptions o;
    o.deg_per_count = kDpc;
    FlickDetector d(o);
    Feeder f{d};
    const double amp = 20.0 / kDpc;  // a 20° flick
    f.flick(dir_deg(0, amp), dir_deg(0, amp));                 // right, landed: good
    f.flick(dir_deg(90, amp), dir_deg(90, amp * 0.85));        // up, 15% short: undershoot
    f.flick(dir_deg(180, amp), dir_deg(180, amp * 1.15));      // left, 15% long: overshoot
    f.flick(dir_deg(-90, amp), dir_deg(-90 + 12, amp));        // down, 12° off: sideways
    auto flicks = d.take();
    REQUIRE(flicks.size() == 4);
    CHECK(flicks[0].kind == LiveFlick::Kind::Good);
    CHECK(flicks[1].kind == LiveFlick::Kind::Undershoot);
    CHECK(flicks[1].error_pct == doctest::Approx(15.0).epsilon(0.15));
    CHECK(flicks[2].kind == LiveFlick::Kind::Overshoot);
    CHECK(flicks[3].kind == LiveFlick::Kind::Sideways);
    CHECK(length(flicks[0].settled) * kDpc == doctest::Approx(20.0).epsilon(0.05));
}

TEST_CASE("live tuning ignores movement that is not an aimed flick") {
    FlickDetectorOptions o;
    o.deg_per_count = kDpc;
    FlickDetector d(o);
    Feeder f{d};
    const double amp = 20.0 / kDpc;
    // A flick that is never shot.
    f.move_to(f.pos + dir_deg(0, amp), 160);
    f.wait(800);
    // A long turn (looking around), then a shot.
    f.move_to(f.pos + dir_deg(180, 170.0 / kDpc), 900);
    f.wait(150);
    f.click();
    f.wait(500);
    // Slow tracking with clicks along the way.
    for (int i = 0; i < 20; ++i) {
        f.move_to(f.pos + dir_deg(0, 0.5 / kDpc), 40);
        f.click();
    }
    f.wait(500);
    // A tiny adjustment (below 3°), then a shot.
    f.move_to(f.pos + dir_deg(90, 1.0 / kDpc), 120);
    f.wait(150);
    f.click();
    f.wait(500);
    CHECK(d.take().empty());
}

TEST_CASE("live tuning nudges the fix toward the directions that fall short, in small bounded steps") {
    FlickDetectorOptions o;
    o.deg_per_count = kDpc;
    FlickDetector d(o);
    Feeder f{d};
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> dir(-180.0, 180.0), dist(14.0, 35.0);
    std::normal_distribution<double> noise(0.0, 0.03);
    LiveTunerOptions to;
    to.overall = false;  // directions only here; the overall speed has its own test
    LiveTuner tuner(to);
    // Down flicks land 6% short, the rest on average right (each flick ±3%).
    for (int i = 0; i < 160; ++i) {
        const double a = dir(rng), deg = dist(rng);
        const double short_by = std::fabs(a + 90.0) < 45.0 ? 0.06 : 0.0;
        const double amp = deg / kDpc;
        f.flick(dir_deg(a, amp), dir_deg(a, amp * (1.0 - short_by + noise(rng))));
        for (const auto& fl : d.take()) tuner.add(fl);
    }
    REQUIRE(tuner.pending() >= 120);
    RawAccelProfile current;
    const auto next = tuner.propose(current);
    REQUIRE(next.has_value());
    const double down = next->gain(-kPi / 2), up = next->gain(kPi / 2), right = next->gain(0.0);
    CHECK(down > up);
    CHECK(down > right);
    CHECK(down - 1.0 <= 0.0105);  // at most 1% per update
    // The average stays the same.
    double s = 0.0;
    for (int i = 0; i < 72; ++i) s += std::log(next->gain(i * kTwoPi / 72));
    CHECK(std::fabs(s / 72.0) < 1e-3);
    CHECK(tuner.pending() == 0);
}

TEST_CASE("live tuning leaves the fix alone when the flicks show no pattern, and never leaves the bounds") {
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> dir(-kPi, kPi);
    std::normal_distribution<double> noise(0.0, 0.04);
    LiveTuner tuner;
    for (int i = 0; i < 120; ++i) {
        const double a = dir(rng);
        LiveFlick fl;
        fl.settled = raw_unit_from_screen_angle(a) * 700.0;
        fl.primary = fl.settled * (1.0 + noise(rng));
        tuner.add(fl);
    }
    CHECK_FALSE(tuner.propose(RawAccelProfile{}).has_value());

    // A fix already at its 8% limit cannot be pushed further.
    RawAccelProfile at_limit;
    at_limit.ud_ratio = 1.08 / 0.98;
    at_limit.sens = 0.98;
    LiveTunerOptions o;
    o.max_step_pct = 5.0;
    LiveTuner pushy(o);
    for (int i = 0; i < 200; ++i) {
        const double a = (i % 2 ? 1.0 : -1.0) * kPi / 2 + (i % 7 - 3) * 0.05;  // up and down
        LiveFlick fl;
        fl.settled = raw_unit_from_screen_angle(a) * 700.0;
        const double short_by = a > 0 ? 0.10 : 0.0;  // up falls 10% short
        fl.primary = fl.settled * (1.0 - short_by + noise(rng) * 0.25);
        pushy.add(fl);
    }
    if (const auto next = pushy.propose(at_limit)) {
        double lg = 0.0;
        for (int i = 0; i < 72; ++i) lg += std::log(next->gain(i * kTwoPi / 72));
        const double overall = std::exp(lg / 72);
        CHECK(std::fabs(overall - 1.0) <= 0.08 + 1e-6);
        for (int i = 0; i < 72; ++i) CHECK(std::fabs(next->gain(i * kTwoPi / 72) / overall - 1.0) <= 0.08 + 1e-6);
        CHECK(std::fabs(next->rotation_deg) <= 3.0 + 1e-9);
    }
}

TEST_CASE("live tuning raises the overall speed slowly when every direction lands short, up to 8%") {
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> dir(-kPi, kPi);
    std::normal_distribution<double> noise(0.0, 0.03);
    auto feed = [&](LiveTuner& t) {
        for (int i = 0; i < 160; ++i) {
            LiveFlick fl;
            fl.settled = raw_unit_from_screen_angle(dir(rng)) * 700.0;
            fl.primary = fl.settled * (0.94 + noise(rng));  // 6% short everywhere
            t.add(fl);
        }
    };
    auto overall_of = [](const RawAccelProfile& p) {
        double lg = 0.0;
        for (int i = 0; i < 72; ++i) lg += std::log(p.gain(i * kTwoPi / 72));
        return std::exp(lg / 72);
    };
    LiveTuner tuner;
    feed(tuner);
    const auto next = tuner.propose(RawAccelProfile{});
    REQUIRE(next.has_value());
    CHECK(overall_of(*next) > 1.004);
    CHECK(overall_of(*next) <= 1.0051);  // at most 0.5% per change
    // The same in every direction: the shape does not change.
    CHECK(next->gain(kPi / 2) / next->gain(0.0) == doctest::Approx(1.0).epsilon(0.004));
    // Many changes later it stops at 8%.
    RawAccelProfile p = *next;
    for (int k = 0; k < 40; ++k) {
        feed(tuner);
        if (const auto n2 = tuner.propose(p)) p = *n2;
    }
    CHECK(overall_of(p) <= 1.08 + 1e-6);
    CHECK(overall_of(p) > 1.07);

    LiveTunerOptions off;
    off.overall = false;
    LiveTuner fixed(off);
    feed(fixed);
    // With the overall speed off it never moves (a chance direction difference may still be nudged).
    if (const auto q = fixed.propose(RawAccelProfile{})) CHECK(overall_of(*q) == doctest::Approx(1.0).epsilon(1e-6));
}
