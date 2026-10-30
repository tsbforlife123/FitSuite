#include "fit/mouse_check.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace fit {

Plan make_check_plan(const CheckPlanOptions& o) {
    Plan p;
    p.kind = SessionKind::Check;
    p.seed = o.seed;
    Rng rng(o.seed ^ 0xC4EC4ull);
    const int dirs = std::max(o.directions, 1);
    // Out-and-back pairs (round format): one outward target per direction, distances alternating
    // and swapped every block, each followed by a target back at the centre.
    for (int b = 0; b < std::max(o.blocks, 1); ++b) {
        std::vector<TargetSpec> outward;
        int i = 0;
        for (int d = 0; d < dirs; ++d) {
            TargetSpec s;
            s.dir_deg = 360.0 * d / dirs;
            s.dist_deg = o.distances_deg[(size_t(d) + size_t(b)) % o.distances_deg.size()];
            s.nominal_dist_deg = s.dist_deg;
            s.radius_deg = o.radius_deg;
            s.block = b;
            s.index = b * 1000 + i++;
            s.condition = o.condition;
            s.purpose = TrialPurpose::Scored;
            outward.push_back(s);
        }
        rng.shuffle(outward);
        p.blocks.push_back(out_and_back(outward));
        p.block_condition.push_back(o.condition);
        BlockInfo bi;
        bi.task = Task::Flick;
        bi.target_timeout_ms = o.target_timeout_ms;
        bi.radius_deg = o.radius_deg;
        p.block_info.push_back(bi);
    }
    p.order_label = std::string(p.blocks.size(), o.condition == Condition::Candidate ? 'B' : 'A');
    return p;
}

const char* to_string(CheckVerdict v) {
    switch (v) {
        case CheckVerdict::StillFits: return "still_fits";
        case CheckVerdict::Changed: return "changed";
        case CheckVerdict::NotEnoughData: return "not_enough_data";
    }
    return "not_enough_data";
}

FitOptions check_fit_options() {
    FitOptions o;
    o.overall_speed = false;  // the quick check looks at directions only
    o.alpha = 0.005;
    o.min_per_direction = 4;
    o.min_total = 32;
    o.min_distances = 2;
    return o;
}

namespace {

// "your down flicks land 9% short" for the direction whose leftover is largest.
std::string worst_direction(const std::vector<DirectionSummary>& dirs) {
    static const char* names[8] = {"right", "up-right", "up", "up-left", "left", "down-left", "down", "down-right"};
    std::vector<double> v;
    for (const auto& d : dirs)
        if (d.n) v.push_back(d.mean_radial_error_pct);
    if (v.empty()) return "";
    std::vector<double> sorted = v;
    std::sort(sorted.begin(), sorted.end());
    const double mid = sorted.size() % 2 ? sorted[sorted.size() / 2]
                                         : 0.5 * (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]);
    int worst = -1;
    double dev = 0.0;
    for (int i = 0; i < int(dirs.size()) && i < 8; ++i)
        if (dirs[size_t(i)].n && std::fabs(dirs[size_t(i)].mean_radial_error_pct - mid) > std::fabs(dev)) {
            dev = dirs[size_t(i)].mean_radial_error_pct - mid;
            worst = i;
        }
    if (worst < 0 || std::fabs(dev) < 1.0) return "";
    return std::string("your ") + names[worst] + " flicks land " + std::to_string(std::lround(std::fabs(dev))) + "% " +
           (dev < 0 ? "shorter" : "further") + " than your typical flick";
}

}  // namespace

DirectionalProfile expected_leftover(const Session& cal, const Transform& fix, const FitOptions& opt) {
    if (cal.kind != SessionKind::Calibration || fix.is_identity()) return {};
    Session s = cal;
    for (auto& t : s.trials) {
        Transformer tr(fix);
        for (auto& m : t.motion) {
            const Vec2 o = tr.apply({double(m.in_dx), double(m.in_dy)}, m.t_us);
            m.out_dx = o.x;
            m.out_dy = o.y;
        }
    }
    // The leftover is only an expectation for the analysis, never applied to anyone's mouse: it may go
    // up to the hard limits (a capped fix leaves more than 8%), and a component is kept on a lenient
    // test (alpha 0.2) with no size gate, so a real leftover is rarely missed while pure noise
    // usually is dropped.
    FitOptions o = opt;
    o.bounds = {kHardMaxGainDeviation, kHardMaxAngleDeg};
    o.alpha = 0.2;
    o.negligible_gain = 0.0;
    o.negligible_angle_deg = 0.0;
    const FitResult r = fit_profiles(s, o);
    if (!r.ok) return {};
    return r.directional;
}

CheckResult check_fix(const Session& s, const DirectionalProfile& expected, const FitOptions& opt) {
    CheckResult r;
    if (s.kind != SessionKind::Check) {
        r.explanation = "This is not a quick-check session.";
        return r;
    }
    // Trial metrics are computed from the output counts, i.e. the crosshair movement with the fix in
    // use. Fitting them as if they were uncorrected calibration flicks measures the bias that is left.
    // The leftover the calibration already predicted (a fix capped at its bounds) is taken out first,
    // per target direction, so only a change since the measurement remains.
    Session as_cal = s;
    as_cal.kind = SessionKind::Calibration;
    const bool adjust = !expected.is_identity();
    for (auto& t : as_cal.trials) {
        t.spec.condition = Condition::Baseline;
        if (!adjust) continue;
        const double theta = t.spec.dir_deg * kDegToRad;
        for (auto& m : t.motion) {
            const Vec2 o = expected.apply({m.out_dx, m.out_dy}, theta);
            m.out_dx = o.x;
            m.out_dy = o.y;
        }
    }
    r.residual = fit_profiles(as_cal, opt);
    if (!r.residual.ok) {
        r.verdict = CheckVerdict::NotEnoughData;
        r.explanation = "Not enough clean flicks to tell (" +
                        std::to_string(r.residual.n_used) + " usable). Run the check again, and flick at your normal speed.";
        return r;
    }
    r.max_residual_pct = r.residual.max_radial_bias_before_pct;
    r.max_residual_angle_deg = r.residual.max_angle_bias_before_deg;
    // A leftover counts only when it is statistically clear and its fitted size matters.
    const FitResult& f = r.residual;
    const bool gain = f.gain_test.significant && f.directional.max_gain_deviation() >= kCheckMinGain;
    const bool angle = f.angle_test.significant && f.directional.max_angle_deg() >= kCheckMinAngleDeg;
    std::ostringstream os;
    if (gain || angle) {
        r.verdict = CheckVerdict::Changed;
        const std::string where = worst_direction(f.by_direction);
        os << "Your aim has changed since you measured";
        if (!where.empty()) os << ": with the fix, " << where;
        else if (angle) os << ": with the fix, some flicks drift sideways";
        os << ". Measure again for a new fix.";
    } else {
        r.verdict = CheckVerdict::StillFits;
        os << "No clear change: with the fix, your flicks land about the same in every direction.";
    }
    r.explanation = os.str();
    return r;
}

}  // namespace fit
