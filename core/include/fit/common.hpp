// FitSuite core — shared small types and helpers. No OS headers.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>

namespace fit {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;
inline constexpr double kDegToRad = kPi / 180.0;
inline constexpr double kRadToDeg = 180.0 / kPi;

// 2-D vector. Unless stated otherwise, vectors are in *raw mouse coordinates*:
// +x = mouse moved right, +y = mouse moved toward the user (screen down).
// This matches Windows raw input and Raw Accel.
struct Vec2 {
    double x = 0.0;
    double y = 0.0;

    constexpr Vec2() = default;
    constexpr Vec2(double x_, double y_) : x(x_), y(y_) {}

    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(double s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(double s) const { return {x / s, y / s}; }
    Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(double s) { x *= s; y *= s; return *this; }
    constexpr bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }
};

inline double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline double length(Vec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }

// Direction angle in "screen convention": 0 = right, +90° = up (mouse pushed away from the user).
// Raw coordinates have +y = down, hence the sign flip.
inline double screen_angle(Vec2 raw) { return std::atan2(-raw.y, raw.x); }

// Unit vector (raw coordinates) for a screen-convention angle.
inline Vec2 raw_unit_from_screen_angle(double theta) { return {std::cos(theta), -std::sin(theta)}; }

// Wrap an angle to (-pi, pi].
inline double wrap_pi(double a) {
    a = std::fmod(a + kPi, kTwoPi);
    if (a < 0) a += kTwoPi;
    return a - kPi;
}

// Wrap degrees to (-180, 180].
inline double wrap_180(double d) { return wrap_pi(d * kDegToRad) * kRadToDeg; }

// Rotate a raw-coordinate vector counter-clockwise *as seen on screen* by angle a (radians).
// (On screen +y is up, so a screen-CCW rotation is a clockwise rotation in raw coordinates.)
inline Vec2 rotate_screen_ccw(Vec2 raw, double a) {
    const double c = std::cos(a), s = std::sin(a);
    // screen coords: (x, -y); rotate CCW; back to raw.
    const double sx = raw.x, sy = -raw.y;
    const double rx = c * sx - s * sy;
    const double ry = s * sx + c * sy;
    return {rx, -ry};
}

// Signed angle (radians) from direction `ref` to vector `v`, positive = counter-clockwise on screen.
inline double signed_screen_angle_between(Vec2 ref, Vec2 v) {
    const double rx = ref.x, ry = -ref.y, vx = v.x, vy = -v.y;
    return std::atan2(rx * vy - ry * vx, rx * vx + ry * vy);
}

inline bool is_finite(double v) { return std::isfinite(v); }

}  // namespace fit
