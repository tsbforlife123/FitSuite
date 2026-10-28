#include <cmath>

#include "doctest.h"
#include "fit/mouse_transform.hpp"
#include "fit/rng.hpp"

using namespace fit;

namespace {

DirectionalProfile random_profile(Rng& r, double scale) {
    DirectionalProfile p;
    for (int k = 0; k < kFourierOrder; ++k) {
        p.gain_cos[k] = r.uniform(-scale, scale);
        p.gain_sin[k] = r.uniform(-scale, scale);
        p.angle_cos[k] = r.uniform(-scale, scale);
        p.angle_sin[k] = r.uniform(-scale, scale);
    }
    p.angle_const = r.uniform(-scale, scale);
    return p;
}

Transform directional(const DirectionalProfile& p) {
    Transform t;
    t.kind = TransformKind::Directional;
    t.directional = p;
    return t;
}

}  // namespace

TEST_CASE("zero input always gives exactly zero output") {
    Rng r(3);
    for (int i = 0; i < 50; ++i) {
        DirectionalProfile p = random_profile(r, 0.2);
        enforce_bounds(p, {});
        Transformer tf(directional(p));
        int64_t t = 0;
        // Move, then stop: every zero report must yield zero, regardless of history.
        for (int k = 0; k < 20; ++k) tf.apply({double(r.below(9)) - 4, double(r.below(9)) - 4}, t += 1000);
        const Vec2 z = tf.apply({0, 0}, t += 1000);
        CHECK(z.x == 0.0);
        CHECK(z.y == 0.0);
        CHECK(p.apply({0, 0}, 1.234).x == 0.0);
    }
    RawAccelProfile ra;
    ra.rotation_deg = 3;
    ra.yx_ratio = 1.1;
    ra.lr_ratio = 0.95;
    ra.ud_ratio = 1.05;
    const Vec2 z = ra.apply({0, 0});
    CHECK(z.x == 0.0);
    CHECK(z.y == 0.0);
}

TEST_CASE("gain and angle stay within bounds for every direction") {
    Rng r(11);
    const Bounds b{0.08, 3.0};
    for (int i = 0; i < 40; ++i) {
        DirectionalProfile p = random_profile(r, 0.4);  // deliberately far too large
        CHECK(enforce_bounds(p, b));
        for (int k = 0; k < 20000; ++k) {
            const double th = r.uniform(-kPi, kPi);
            CHECK(std::fabs(p.gain(th) - 1.0) <= b.max_gain_deviation + 1e-9);
            CHECK(std::fabs(p.angle(th)) * kRadToDeg <= b.max_angle_deg + 1e-9);
        }
    }
    // Hard limits cannot be exceeded even if requested.
    DirectionalProfile p = random_profile(r, 0.5);
    enforce_bounds(p, {0.5, 30.0});
    CHECK(p.max_gain_deviation() <= kHardMaxGainDeviation + 1e-6);
    CHECK(p.max_angle_deg() <= kHardMaxAngleDeg + 1e-6);
}

TEST_CASE("bounded output magnitude: (1 - bound)|in| <= |out| <= (1 + bound)|in|") {
    Rng r(5);
    DirectionalProfile p = random_profile(r, 0.3);
    enforce_bounds(p, {0.08, 3.0});
    Transformer tf(directional(p));
    int64_t t = 0;
    for (int i = 0; i < 5000; ++i) {
        const Vec2 in{r.uniform(-50, 50), r.uniform(-50, 50)};
        const Vec2 out = tf.apply(in, t += 1000);
        CHECK(length(out) <= length(in) * (1.0 + 0.08) + 1e-9);
        CHECK(length(out) >= length(in) * (1.0 - 0.08) - 1e-9);
    }
}

TEST_CASE("directional function is continuous, including across ±180°") {
    Rng r(9);
    DirectionalProfile p = random_profile(r, 0.3);
    enforce_bounds(p, {0.08, 3.0});
    const double eps = 1e-6;
    for (int i = 0; i < 2000; ++i) {
        const double th = r.uniform(-kPi, kPi);
        CHECK(std::fabs(p.gain(th + eps) - p.gain(th)) < 1e-5);
        CHECK(std::fabs(p.angle(th + eps) - p.angle(th)) < 1e-5);
    }
    // Wraparound: just below +180° and just above -180° are neighbours.
    CHECK(std::fabs(p.gain(kPi - 1e-9) - p.gain(-kPi + 1e-9)) < 1e-6);
    CHECK(std::fabs(p.angle(kPi - 1e-9) - p.angle(-kPi + 1e-9)) < 1e-6);
    // Lipschitz bound: no abrupt change between neighbouring directions (0.1° apart).
    double worst = 0.0;
    for (int i = 0; i < 3600; ++i) {
        const double a = -kPi + i * kTwoPi / 3600.0;
        worst = std::max(worst, std::fabs(p.gain(a + kTwoPi / 3600.0) - p.gain(a)));
    }
    CHECK(worst < 0.01);
}

TEST_CASE("strength scales the correction; strength 0 is the identity") {
    Rng r(2);
    DirectionalProfile p = random_profile(r, 0.05);
    enforce_bounds(p, {});
    p.strength = 0.0;
    CHECK(p.is_identity());
    const Vec2 in{17, -9};
    const Vec2 out = p.apply(in, screen_angle(in));
    CHECK(out.x == doctest::Approx(in.x));
    CHECK(out.y == doctest::Approx(in.y));
    p.strength = 0.5;
    const double half = p.log_gain(0.7);
    p.strength = 1.0;
    CHECK(p.log_gain(0.7) == doctest::Approx(2.0 * half));
}

TEST_CASE("memoryless transform is homogeneous: f(c·v) = c·f(v)") {
    Rng r(4);
    DirectionalProfile p = random_profile(r, 0.2);
    enforce_bounds(p, {});
    for (int i = 0; i < 200; ++i) {
        const Vec2 v{r.uniform(-10, 10), r.uniform(-10, 10)};
        const double c = r.uniform(0.1, 20.0);
        const Vec2 a = p.apply(v * c, screen_angle(v));
        const Vec2 b = p.apply(v, screen_angle(v)) * c;
        CHECK(a.x == doctest::Approx(b.x));
        CHECK(a.y == doctest::Approx(b.y));
    }
}

TEST_CASE("slow diagonal movement gets the correction for its true direction") {
    // A pure gain profile: +8% at 0°, -8% at 90° (cos 2θ).
    DirectionalProfile p;
    p.gain_cos[1] = std::log(1.08);
    Transformer tf(directional(p));
    // Reports alternate (1,0),(1,-1): the true direction is atan2(0.5, 1) ≈ 26.6° up-right.
    int64_t t = 0;
    Vec2 in_sum{}, out_sum{};
    for (int i = 0; i < 400; ++i) {
        const Vec2 in = (i % 2 == 0) ? Vec2{1, 0} : Vec2{1, -1};
        out_sum += tf.apply(in, t += 1000);
        in_sum += in;
    }
    const double true_dir = screen_angle(in_sum);
    const double expected = p.gain(true_dir);
    CHECK(length(out_sum) / length(in_sum) == doctest::Approx(expected).epsilon(0.005));
    // Per-report direction would have averaged the 0° and 45° gains instead.
}

TEST_CASE("Raw Accel transcription matches its pipeline") {
    RawAccelProfile ra;
    // Positive rotation in raw coordinates turns right (+x) toward +y (down on screen).
    ra.rotation_deg = 90.0;
    Vec2 v = ra.apply({1, 0});
    CHECK(v.x == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(v.y == doctest::Approx(1.0));
    // Up multiplier applies only to y < 0 (moving up), left multiplier only to x < 0.
    RawAccelProfile m;
    m.ud_ratio = 1.1;
    m.lr_ratio = 0.9;
    m.yx_ratio = 1.2;
    m.sens = 2.0;
    CHECK(m.apply({0, -10}).y == doctest::Approx(-10 * 2.0 * 1.2 * 1.1));
    CHECK(m.apply({0, 10}).y == doctest::Approx(10 * 2.0 * 1.2));
    CHECK(m.apply({-10, 0}).x == doctest::Approx(-10 * 2.0 * 0.9));
    CHECK(m.apply({10, 0}).x == doctest::Approx(10 * 2.0));
    // Gain/angle helpers: Y/X ratio 1.2 bends a 45° movement toward vertical.
    RawAccelProfile yx;
    yx.yx_ratio = 1.2;
    CHECK(yx.angle(45 * kDegToRad) * kRadToDeg == doctest::Approx(std::atan(1.2) * kRadToDeg - 45.0));
    CHECK(yx.gain(90 * kDegToRad) == doctest::Approx(1.2));
}

TEST_CASE("identity transformer passes input through untouched") {
    Transformer tf;
    const Vec2 o = tf.apply({123, -45}, 1000);
    CHECK(o.x == 123.0);
    CHECK(o.y == -45.0);
}
