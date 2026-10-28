// MouseFit quick check: about a minute of flicks with the player's fix in use, analysed for any
// directional bias that is left. It answers one question: does the fix still fit, or has the
// player's aim changed enough that a new measurement is worth it?
//
// The flicks are analysed in output space (what moved the crosshair, with the fix applied) with the
// calibration model. "Changed" needs both a clear statistical signal (alpha 0.005, stricter than a
// calibration because a short session gives the tests less data) and a fitted leftover of at least
// 5% in some direction or 2.5° of angle, measured against the leftover the fix's own calibration
// predicts (a fix capped at its bounds leaves some). With simulated players playing it in rounds
// (tests/test_check.cpp) it calls an unchanged player "changed" in 1–4% of checks (also with a
// capped fix), notices a new 12% downward undershoot about seven times in ten and a doubled vertical
// undershoot about nine times in ten. The check
// never changes the fix itself: a new fix always comes from a full measurement and a blind test.
#pragma once

#include <string>

#include "fit/mouse_fit.hpp"
#include "fit/mouse_schedule.hpp"

namespace fit {

struct CheckPlanOptions {
    std::vector<double> distances_deg{10.0, 22.0};
    int directions = 8;
    int blocks = 3;  // 3 × 16 = 48 targets (8 out and 8 back each), about a minute
    double radius_deg = 1.2;
    double target_timeout_ms = 2200.0;
    // Candidate: FitSuite applies the fix during the check (Raw Accel is on the player's own
    // settings while FitSuite is in front). Baseline: Raw Accel already applies the fix everywhere.
    Condition condition = Condition::Candidate;
    uint64_t seed = 3;
};

Plan make_check_plan(const CheckPlanOptions& o);

enum class CheckVerdict { StillFits, Changed, NotEnoughData };
const char* to_string(CheckVerdict v);  // "still_fits" | "changed" | "not_enough_data"

struct CheckResult {
    CheckVerdict verdict = CheckVerdict::NotEnoughData;
    std::string explanation;             // plain language, for the player
    double max_residual_pct = 0.0;       // largest per-direction distance bias left, vs the player's average
    double max_residual_angle_deg = 0.0; // largest per-direction angle bias left
    FitResult residual;                  // the fit on the check flicks (with the fix in use)
};

// Fit options for the short check: the calibration model with a stricter alpha and minimum
// sample sizes that suit 48 flicks.
FitOptions check_fit_options();

// Size a leftover has to reach before the check calls it a change.
inline constexpr double kCheckMinGain = 0.05;      // 5% distance in some direction
inline constexpr double kCheckMinAngleDeg = 2.5;   // below the 3° bound, so a rotation can still be seen

// The correction a calibration would still ask for with `fix` in use: identity when the fix covers
// the whole bias, the part that is left when the bias was larger than the fix's bounds allow (or
// than Raw Accel's settings can express). Found by applying the fix to the calibration flicks and
// fitting them again.
DirectionalProfile expected_leftover(const Session& calibration, const Transform& fix, const FitOptions& opt = {});

// `expected`: expected_leftover() of the fix's own calibration. The check asks whether the flicks
// with the fix differ from what that calibration predicts, so a fix that was capped at the bounds
// is not reported as a change in aim.
CheckResult check_fix(const Session& check, const DirectionalProfile& expected = {},
                      const FitOptions& opt = check_fit_options());

}  // namespace fit
