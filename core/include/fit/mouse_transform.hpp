// MouseFit directional transforms.
//
// Two families, both mapping raw mouse counts to output counts (raw coordinates, +y = down):
//
//  * DirectionalProfile ("smooth directional", in-app only today)
//        out = g(θ̂) · R(δ(θ̂)) · in
//    θ̂ is the screen-convention direction of the current movement, g a positive gain and δ a small
//    rotation. log g and δ are 2nd-order Fourier series in θ, so both are periodic (no seam at ±180°)
//    and smooth. log g has no constant term: averaged over directions, sensitivity is unchanged.
//
//  * RawAccelProfile ("Raw Accel-compatible")
//    A transcription of Raw Accel 1.7's no-acceleration pipeline (common/rawaccel.hpp, modify()):
//        rotate by degrees_rotation → ×sens on x, ×sens·yx on y → ×lr if x<0 → ×ud if y<0
//    so the transform validated in FitSuite is the one Raw Accel would apply with those values.
//
// Both are linear in the current input for a fixed direction, so zero input always yields zero output
// and neither can produce motion on its own.
#pragma once

#include <array>
#include <string>

#include "fit/common.hpp"

namespace fit {

inline constexpr int kFourierOrder = 2;

struct Bounds {
    double max_gain_deviation = 0.08;  // |g - 1| <= this for every direction
    double max_angle_deg = 3.0;        // |δ| <= this for every direction
};
inline constexpr double kHardMaxGainDeviation = 0.15;
inline constexpr double kHardMaxAngleDeg = 6.0;

struct DirectionalProfile {
    // log g(θ) = Σ_{k=1..2} gain_cos[k-1]·cos kθ + gain_sin[k-1]·sin kθ
    std::array<double, kFourierOrder> gain_cos{};
    std::array<double, kFourierOrder> gain_sin{};
    // δ(θ) (radians) = angle_const + Σ_{k=1..2} angle_cos[k-1]·cos kθ + angle_sin[k-1]·sin kθ
    double angle_const = 0.0;
    std::array<double, kFourierOrder> angle_cos{};
    std::array<double, kFourierOrder> angle_sin{};
    // User-chosen strength in [0, 1]; scales both log g and δ (0 = identity).
    double strength = 1.0;

    double log_gain(double theta) const;   // includes strength
    double gain(double theta) const { return std::exp(log_gain(theta)); }
    double angle(double theta) const;      // radians, includes strength
    bool is_identity() const;

    // Largest |g-1| and |δ| (degrees) over a dense direction grid.
    double max_gain_deviation() const;
    double max_angle_deg() const;

    // Apply the transform for a known movement direction (memoryless).
    Vec2 apply(Vec2 in, double theta) const;
};

// Scale (never clip) the gain and angle parts so the profile satisfies `b`. Scaling keeps the
// function smooth; pointwise clipping would create kinks. Returns true if anything was scaled.
bool enforce_bounds(DirectionalProfile& p, Bounds b);

struct RawAccelProfile {
    double rotation_deg = 0.0;  // "Degrees of rotation"
    double sens = 1.0;          // "Output DPI" / 1000 (no DPI normalization)
    double yx_ratio = 1.0;      // "Y/X output DPI ratio (vertical sens multiplier)"
    double lr_ratio = 1.0;      // "L/R output DPI ratio (left sens multiplier)"
    double ud_ratio = 1.0;      // "U/D output DPI ratio (up sens multiplier)"

    Vec2 apply(Vec2 in) const;
    // Exact inverse of apply() (the sign tests are unaffected by the positive scalings).
    Vec2 inverse_apply(Vec2 out) const;
    bool is_identity() const;
    // Radial gain and output-direction change for a unit input in screen direction theta.
    double gain(double theta) const;
    double angle(double theta) const;  // radians, screen CCW positive
    bool uses_asymmetry() const { return lr_ratio != 1.0 || ud_ratio != 1.0; }
};

enum class TransformKind { Identity, Directional, RawAccel };

const char* to_string(TransformKind k);

// A transform as used in a test session (what the crosshair sees).
struct Transform {
    TransformKind kind = TransformKind::Identity;
    DirectionalProfile directional;
    RawAccelProfile raw_accel;

    static Transform identity() { return {}; }
    bool is_identity() const;
};

// Stateful applier used per input report. For the directional family it tracks the movement
// direction with a time-weighted sum of recent input (half-life `direction_halflife_ms`), so a slow
// diagonal movement made of (1,0),(1,1),(1,0)… reports gets the correction for its true direction
// rather than alternating between the 0° and 45° corrections.
class Transformer {
public:
    explicit Transformer(Transform t = Transform::identity(), double direction_halflife_ms = 3.0);

    void set(Transform t);
    const Transform& transform() const { return t_; }
    void reset();

    // t_us: report timestamp in microseconds (monotonic). Zero input returns exactly zero.
    Vec2 apply(Vec2 in, int64_t t_us);

private:
    Transform t_;
    double halflife_ms_;
    Vec2 trend_{};
    int64_t last_t_us_ = 0;
    bool has_last_ = false;
};

}  // namespace fit
