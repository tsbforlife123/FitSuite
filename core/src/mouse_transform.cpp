#include "fit/mouse_transform.hpp"

#include <algorithm>
#include <cmath>

namespace fit {

namespace {
constexpr int kGrid = 3600;          // 0.1° spacing for bound checks
constexpr double kBoundMargin = 0.995;  // keep strictly inside the bound between grid points

double raw_log_gain(const DirectionalProfile& p, double th) {
    double s = 0.0;
    for (int k = 0; k < kFourierOrder; ++k) {
        const double m = double(k + 1);
        s += p.gain_cos[k] * std::cos(m * th) + p.gain_sin[k] * std::sin(m * th);
    }
    return s;
}

double raw_angle(const DirectionalProfile& p, double th) {
    double s = p.angle_const;
    for (int k = 0; k < kFourierOrder; ++k) {
        const double m = double(k + 1);
        s += p.angle_cos[k] * std::cos(m * th) + p.angle_sin[k] * std::sin(m * th);
    }
    return s;
}

double grid_theta(int i) { return -kPi + (double(i) + 0.5) * kTwoPi / double(kGrid); }
}  // namespace

double DirectionalProfile::log_gain(double theta) const { return strength * raw_log_gain(*this, theta); }
double DirectionalProfile::angle(double theta) const { return strength * raw_angle(*this, theta); }

bool DirectionalProfile::is_identity() const {
    if (strength == 0.0) return true;
    for (int k = 0; k < kFourierOrder; ++k)
        if (gain_cos[k] != 0 || gain_sin[k] != 0 || angle_cos[k] != 0 || angle_sin[k] != 0) return false;
    return angle_const == 0.0;
}

double DirectionalProfile::max_gain_deviation() const {
    double m = 0.0;
    for (int i = 0; i < kGrid; ++i) m = std::max(m, std::fabs(gain(grid_theta(i)) - 1.0));
    return m;
}

double DirectionalProfile::max_angle_deg() const {
    double m = 0.0;
    for (int i = 0; i < kGrid; ++i) m = std::max(m, std::fabs(angle(grid_theta(i))));
    return m * kRadToDeg;
}

Vec2 DirectionalProfile::apply(Vec2 in, double theta) const {
    if (in.x == 0.0 && in.y == 0.0) return {0.0, 0.0};
    const double g = gain(theta);
    const double d = angle(theta);
    return rotate_screen_ccw(in, d) * g;
}

bool enforce_bounds(DirectionalProfile& p, Bounds b) {
    b.max_gain_deviation = std::clamp(b.max_gain_deviation, 0.0, kHardMaxGainDeviation);
    b.max_angle_deg = std::clamp(b.max_angle_deg, 0.0, kHardMaxAngleDeg);
    bool changed = false;

    // Gain: bound is enforced at full strength, so any strength <= 1 also satisfies it
    // (|exp(s·L) - 1| grows monotonically with s >= 0 for either sign of L).
    auto gain_dev_at = [&](double s) {
        double m = 0.0;
        for (int i = 0; i < kGrid; ++i) m = std::max(m, std::fabs(std::exp(s * raw_log_gain(p, grid_theta(i))) - 1.0));
        return m;
    };
    const double gbound = b.max_gain_deviation * kBoundMargin;
    if (gain_dev_at(1.0) > gbound) {
        double lo = 0.0, hi = 1.0;
        for (int it = 0; it < 60; ++it) {
            const double mid = 0.5 * (lo + hi);
            if (gain_dev_at(mid) > gbound) hi = mid; else lo = mid;
        }
        for (int k = 0; k < kFourierOrder; ++k) { p.gain_cos[k] *= lo; p.gain_sin[k] *= lo; }
        changed = true;
    }

    double amax = 0.0;
    for (int i = 0; i < kGrid; ++i) amax = std::max(amax, std::fabs(raw_angle(p, grid_theta(i))));
    const double abound = b.max_angle_deg * kDegToRad * kBoundMargin;
    if (amax > abound && amax > 0.0) {
        const double s = abound / amax;
        p.angle_const *= s;
        for (int k = 0; k < kFourierOrder; ++k) { p.angle_cos[k] *= s; p.angle_sin[k] *= s; }
        changed = true;
    }
    p.strength = std::clamp(p.strength, 0.0, 1.0);
    return changed;
}

// ---- Raw Accel ---------------------------------------------------------------------------

Vec2 RawAccelProfile::apply(Vec2 in) const {
    Vec2 v = in;
    if (rotation_deg != 0.0) {
        // rawaccel: direction(deg) = (cos, sin); rotate(v, dir) = (x·c − y·s, x·s + y·c)
        const double r = rotation_deg * kDegToRad;
        const double c = std::cos(r), s = std::sin(r);
        v = {v.x * c - v.y * s, v.x * s + v.y * c};
    }
    v.x *= sens;
    v.y *= sens * yx_ratio;
    if (lr_ratio != 1.0 && v.x < 0) v.x *= lr_ratio;
    if (ud_ratio != 1.0 && v.y < 0) v.y *= ud_ratio;
    return v;
}

Vec2 RawAccelProfile::inverse_apply(Vec2 out) const {
    Vec2 v = out;
    if (ud_ratio != 1.0 && v.y < 0) v.y /= ud_ratio;
    if (lr_ratio != 1.0 && v.x < 0) v.x /= lr_ratio;
    v.x /= sens;
    v.y /= sens * yx_ratio;
    if (rotation_deg != 0.0) {
        const double r = -rotation_deg * kDegToRad;
        const double c = std::cos(r), s = std::sin(r);
        v = {v.x * c - v.y * s, v.x * s + v.y * c};
    }
    return v;
}

bool RawAccelProfile::is_identity() const {
    return rotation_deg == 0.0 && sens == 1.0 && yx_ratio == 1.0 && lr_ratio == 1.0 && ud_ratio == 1.0;
}

double RawAccelProfile::gain(double theta) const { return length(apply(raw_unit_from_screen_angle(theta))); }

double RawAccelProfile::angle(double theta) const {
    const Vec2 u = raw_unit_from_screen_angle(theta);
    return signed_screen_angle_between(u, apply(u));
}

const char* to_string(TransformKind k) {
    switch (k) {
        case TransformKind::Identity: return "identity";
        case TransformKind::Directional: return "directional";
        case TransformKind::RawAccel: return "rawaccel";
    }
    return "identity";
}

bool Transform::is_identity() const {
    switch (kind) {
        case TransformKind::Identity: return true;
        case TransformKind::Directional: return directional.is_identity();
        case TransformKind::RawAccel: return raw_accel.is_identity();
    }
    return true;
}

// ---- Transformer -------------------------------------------------------------------------

Transformer::Transformer(Transform t, double direction_halflife_ms)
    : t_(std::move(t)), halflife_ms_(std::max(direction_halflife_ms, 0.1)) {}

void Transformer::set(Transform t) {
    t_ = std::move(t);
    reset();
}

void Transformer::reset() {
    trend_ = {};
    has_last_ = false;
    last_t_us_ = 0;
}

Vec2 Transformer::apply(Vec2 in, int64_t t_us) {
    if (in.x == 0.0 && in.y == 0.0) return {0.0, 0.0};
    switch (t_.kind) {
        case TransformKind::Identity: return in;
        case TransformKind::RawAccel: return t_.raw_accel.apply(in);
        case TransformKind::Directional: break;
    }
    if (has_last_) {
        const double dt_ms = std::max(0.0, double(t_us - last_t_us_) / 1000.0);
        const double decay = std::exp2(-dt_ms / halflife_ms_);
        trend_ = trend_ * decay + in;
    } else {
        trend_ = in;
    }
    has_last_ = true;
    last_t_us_ = t_us;
    Vec2 dir = trend_;
    if (length(dir) < 1e-9) dir = in;
    return t_.directional.apply(in, screen_angle(dir));
}

}  // namespace fit
