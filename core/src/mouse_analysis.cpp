#include "fit/mouse_analysis.hpp"

#include <algorithm>
#include <cmath>

#include "fit/stats.hpp"

namespace fit {

const char* to_string(Condition c) { return c == Condition::Baseline ? "baseline" : "candidate"; }
const char* to_string(TrialPurpose p) {
    switch (p) {
        case TrialPurpose::Scored: return "scored";
        case TrialPurpose::Recenter: return "recenter";
        case TrialPurpose::Warmup: return "warmup";
    }
    return "scored";
}
const char* to_string(TrialOutcome o) {
    switch (o) {
        case TrialOutcome::Hit: return "hit";
        case TrialOutcome::Timeout: return "timeout";
        case TrialOutcome::Interrupted: return "interrupted";
        case TrialOutcome::Miss: return "miss";
    }
    return "interrupted";
}
const char* to_string(Task t) {
    switch (t) {
        case Task::Flick: return "flick";
        case Task::Micro: return "micro";
        case Task::Switch: return "switch";
        case Task::Track: return "track";
    }
    return "flick";
}
const char* to_string(SessionKind k) {
    return k == SessionKind::Calibration ? "calibration" : k == SessionKind::Validation ? "validation" : "check";
}
const char* to_string(FlickClass c) {
    switch (c) {
        case FlickClass::Undershoot: return "undershoot";
        case FlickClass::OnTarget: return "on-target";
        case FlickClass::Overshoot: return "overshoot";
    }
    return "on-target";
}
const char* to_string(PrimaryEndRule r) {
    switch (r) {
        case PrimaryEndRule::None: return "none";
        case PrimaryEndRule::Reversal: return "reversal";
        case PrimaryEndRule::Stop: return "stop";
        case PrimaryEndRule::Reacceleration: return "re-acceleration";
        case PrimaryEndRule::Click: return "click";
        case PrimaryEndRule::WindowEnd: return "window-end";
    }
    return "none";
}

namespace {

struct Knot {
    double t;
    Vec2 p;
};

double median_report_interval_ms(const std::vector<MotionSample>& s) {
    std::vector<double> iv;
    for (size_t i = 1; i < s.size(); ++i) {
        const double d = double(s[i].t_us - s[i - 1].t_us) / 1000.0;
        if (d > 0.0 && d < 20.0) iv.push_back(d);
    }
    if (iv.empty()) return 1.0;
    return median(iv);
}

Vec2 interp(const std::vector<Knot>& k, size_t& cursor, double t) {
    while (cursor + 1 < k.size() && k[cursor + 1].t < t) ++cursor;
    if (cursor + 1 >= k.size()) return k.back().p;
    const Knot& a = k[cursor];
    const Knot& b = k[cursor + 1];
    if (t <= a.t) return a.p;
    const double span = b.t - a.t;
    if (span <= 0.0) return b.p;
    const double f = (t - a.t) / span;
    return a.p + (b.p - a.p) * f;
}

void fail(TrialMetrics& m, const char* why) {
    m.valid = false;
    if (m.invalid_reason.empty()) m.invalid_reason = why;
}

}  // namespace

TrialMetrics analyze_trial(const Trial& tr, const GameMapping& mapping, const SegmentParams& P) {
    TrialMetrics m;
    m.spec = tr.spec;
    m.start_pitch_deg = tr.start_pitch_deg;
    m.target_dist_counts = length(tr.target_counts);
    m.target_angle_rad = screen_angle(tr.target_counts);
    m.valid = true;

    const double k = mapping.deg_per_count();
    if (tr.outcome == TrialOutcome::Interrupted) { fail(m, "interrupted"); return m; }
    if (!(k > 0.0) || !(m.target_dist_counts > 0.0)) { fail(m, "invalid mapping or target"); return m; }
    const double window_ms = double(tr.t_end_us - tr.t_appear_us) / 1000.0;
    if (!(window_ms > 0.0)) { fail(m, "empty trial window"); return m; }

    // ---- clicks -------------------------------------------------------------------------------
    double t_hit_ms = -1.0;
    for (const auto& c : tr.clicks) {
        if (c.hit) { t_hit_ms = double(c.t_us - tr.t_appear_us) / 1000.0; break; }
        ++m.misses;
    }
    m.hit = tr.outcome == TrialOutcome::Hit && t_hit_ms >= 0.0;
    if (!m.hit) t_hit_ms = -1.0;
    m.first_click_hit = !tr.clicks.empty() && tr.clicks.front().hit;
    m.ended_by_miss = tr.outcome == TrialOutcome::Miss;
    // A miss (the target disappears) counts like running out of time, so missing is never "faster".
    m.acquisition_ms = m.hit ? t_hit_ms : std::max(window_ms, tr.outcome == TrialOutcome::Miss ? tr.timeout_ms : 0.0);

    // ---- cumulative output position as a piecewise-linear curve -------------------------------
    const double poll_ms = std::clamp(median_report_interval_ms(tr.motion), 0.125, 8.0);
    std::vector<Knot> knots;
    knots.reserve(tr.motion.size() * 2 + 2);
    knots.push_back({0.0, {}});
    Vec2 p{};
    for (const auto& s : tr.motion) {
        const double t = std::max(0.0, double(s.t_us - tr.t_appear_us) / 1000.0);
        const double prev_t = knots.back().t;
        // After a pause, a report's motion happened during the preceding polling interval,
        // not spread over the whole pause.
        if (t - prev_t > 1.5 * poll_ms) knots.push_back({t - poll_ms, p});
        p += Vec2{s.out_dx, s.out_dy};
        knots.push_back({std::max(t, knots.back().t), p});
    }
    if (knots.back().t < window_ms) knots.push_back({window_ms, p});

    const double dt = P.resample_ms;
    const size_t n = size_t(std::floor(window_ms / dt)) + 1;
    if (n < 5) { fail(m, "trial too short"); return m; }
    std::vector<Vec2> pos(n);
    size_t cur = 0;
    for (size_t i = 0; i < n; ++i) pos[i] = interp(knots, cur, double(i) * dt);

    std::vector<Vec2> vel(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t a = i == 0 ? 0 : i - 1;
        const size_t b = i + 1 >= n ? n - 1 : i + 1;
        vel[i] = (pos[b] - pos[a]) / (double(b - a) * dt);  // counts per ms
    }
    // Gaussian smoothing (edge-renormalised).
    const double sig = std::max(P.smooth_sigma_ms / dt, 0.5);
    const int rad = int(std::ceil(3.0 * sig));
    std::vector<double> w(size_t(2 * rad + 1));
    for (int j = -rad; j <= rad; ++j) w[size_t(j + rad)] = std::exp(-0.5 * (j / sig) * (j / sig));
    std::vector<Vec2> vs(n);
    for (size_t i = 0; i < n; ++i) {
        Vec2 acc{};
        double ws = 0.0;
        for (int j = -rad; j <= rad; ++j) {
            const long idx = long(i) + j;
            if (idx < 0 || idx >= long(n)) continue;
            acc += vel[size_t(idx)] * w[size_t(j + rad)];
            ws += w[size_t(j + rad)];
        }
        vs[i] = acc / ws;
    }
    std::vector<double> speed(n), along(n);
    for (size_t i = 0; i < n; ++i) speed[i] = length(vs[i]);

    auto idx_of = [&](double t_ms) {
        return std::min(n - 1, size_t(std::max(0.0, std::round(t_ms / dt))));
    };
    const size_t i_lim = m.hit ? idx_of(t_hit_ms) : n - 1;

    // ---- peak, onset --------------------------------------------------------------------------
    double peak = 0.0;
    size_t i_max = 0;
    for (size_t i = 0; i <= i_lim; ++i)
        if (speed[i] > peak) { peak = speed[i]; i_max = i; }
    m.peak_speed_deg_s = peak * k * 1000.0;
    if (m.peak_speed_deg_s < P.min_peak_deg_s) { fail(m, "no flick detected"); return m; }

    size_t i_pk = i_max;
    for (size_t i = 1; i + 1 <= i_lim && i + 1 < n; ++i) {
        if (speed[i] >= 0.5 * peak && speed[i] >= speed[i - 1] && speed[i] >= speed[i + 1]) { i_pk = i; break; }
    }
    const double pk = speed[i_pk];
    // Onset: walk back from the peak while the speed keeps falling, until it is below 5% of the peak.
    // Stopping at a speed minimum handles round-based tests, where the hand may still be settling
    // from the previous target when this one appears.
    size_t i_on = i_pk;
    while (i_on > 0 && speed[i_on - 1] >= P.onset_frac * peak && speed[i_on - 1] <= speed[i_on]) --i_on;
    m.reaction_ms = double(i_on) * dt;

    // Everything below is measured from where the crosshair was at movement onset.
    const Vec2 p_on = pos[i_on];
    const Vec2 target_from_on = tr.target_counts - p_on;
    const double dist_on = length(target_from_on);
    if (!(dist_on > 0.0)) { fail(m, "already on the target at movement onset"); return m; }
    m.target_dist_counts = dist_on;
    m.target_angle_rad = screen_angle(target_from_on);
    const Vec2 u = target_from_on / dist_on;
    for (size_t i = 0; i < n; ++i) along[i] = dot(vs[i], u);

    // ---- end of the primary submovement -------------------------------------------------------
    size_t i_end = i_lim;
    PrimaryEndRule rule = PrimaryEndRule::WindowEnd;
    const bool toward_target = along[i_pk] > 0.0;
    const size_t reaccel_win = size_t(std::ceil(P.reaccel_window_ms / dt));
    for (size_t i = i_pk + 1; i <= i_lim && i < n; ++i) {
        if (toward_target && along[i] <= 0.0) { i_end = i; rule = PrimaryEndRule::Reversal; break; }
        if (speed[i] < P.stop_frac * peak) { i_end = i; rule = PrimaryEndRule::Stop; break; }
        if (i + 1 < n && speed[i] <= speed[i - 1] && speed[i] <= speed[i + 1] && speed[i] < P.reaccel_min_frac * pk) {
            double later = speed[i];
            for (size_t j = i + 1; j < n && j <= i + reaccel_win; ++j) later = std::max(later, speed[j]);
            if (later >= speed[i] + P.reaccel_rise_frac * peak) {
                i_end = i;
                rule = PrimaryEndRule::Reacceleration;
                break;
            }
        }
    }
    for (const auto& c : tr.clicks) {
        const double tc = double(c.t_us - tr.t_appear_us) / 1000.0;
        const size_t ic = idx_of(tc);
        if (ic >= i_on && ic <= i_end) { i_end = ic; rule = PrimaryEndRule::Click; }
        if (ic >= i_on) break;  // only the first click after onset matters
    }
    m.end_rule = rule;
    m.primary_ms = double(i_end - i_on) * dt;
    m.correction_ms = m.hit ? std::max(0.0, t_hit_ms - double(i_end) * dt) : 0.0;

    // ---- first-flick errors -------------------------------------------------------------------
    const Vec2 e = pos[i_end] - p_on;
    m.endpoint_counts = e;
    const double along_e = dot(e, u);
    m.radial_ratio = along_e / m.target_dist_counts;
    m.radial_error_pct = (m.radial_ratio - 1.0) * 100.0;
    m.angle_error_deg = length(e) > 0.0 ? signed_screen_angle_between(target_from_on, e) * kRadToDeg : 0.0;
    m.endpoint_error_deg = length(e - target_from_on) * k;
    if (m.endpoint_error_deg <= tr.spec.radius_deg) m.flick_class = FlickClass::OnTarget;
    else m.flick_class = m.radial_ratio > 1.0 ? FlickClass::Overshoot : FlickClass::Undershoot;

    // ---- shape of the first flick's path ------------------------------------------------------
    // Sideways offset from the straight line start → end, on screen (+ = left of the line, looking
    // along it), and the path length, from the recorded positions between onset and the flick's end.
    if (const double len_e = length(e); len_e > 0.0) {
        const Vec2 ue = e / len_e;
        double max_off = 0.0, path = 0.0;
        for (size_t i = i_on; i <= i_end && i < n; ++i) {
            const Vec2 d = pos[i] - p_on;
            const double off = d.x * ue.y - d.y * ue.x;  // raw +y is screen down, hence this sign
            if (std::fabs(off) > std::fabs(max_off)) max_off = off;
            if (i > i_on) path += length(pos[i] - pos[i - 1]);
        }
        m.curve_pct = 100.0 * max_off / len_e;
        m.straightness_pct = path > 0.0 ? std::min(100.0, 100.0 * len_e / path) : 100.0;
    }

    // ---- validity -----------------------------------------------------------------------------
    // In a switching round every target was already visible, so moving at once is not a guess.
    if (tr.spec.task != Task::Switch && m.reaction_ms < P.anticipation_ms)
        fail(m, "movement started before the target could be seen");
    const double t_on_us = double(tr.t_appear_us) + m.reaction_ms * 1000.0;
    const double t_end_us = double(tr.t_appear_us) + double(i_end) * dt * 1000.0;
    for (size_t i = 1; i < tr.motion.size(); ++i) {
        const double a = double(tr.motion[i - 1].t_us), b = double(tr.motion[i].t_us);
        // Only silences that start after movement onset: the quiet before a flick is not a gap.
        if (a < t_on_us || a > t_end_us) continue;
        if ((b - a) / 1000.0 > P.max_gap_ms) { fail(m, "gap in the input stream during the flick"); break; }
    }
    return m;
}

double estimate_polling_hz(const std::vector<Trial>& trials) {
    std::vector<double> iv;
    for (const auto& t : trials)
        for (size_t i = 1; i < t.motion.size(); ++i) {
            const auto& a = t.motion[i - 1];
            const auto& b = t.motion[i];
            if ((a.in_dx == 0 && a.in_dy == 0) || (b.in_dx == 0 && b.in_dy == 0)) continue;
            const double d = double(b.t_us - a.t_us) / 1000.0;
            if (d > 0.0 && d < 20.0) iv.push_back(d);
        }
    if (iv.size() < 50) return 0.0;
    const double med = median(iv);
    return med > 0.0 ? 1000.0 / med : 0.0;
}

std::vector<DirectionSummary> summarize_by_direction(const std::vector<TrialMetrics>& ms, int directions) {
    directions = std::max(directions, 1);
    const double step = 360.0 / directions;
    std::vector<std::vector<const TrialMetrics*>> bins(static_cast<size_t>(directions));
    for (const auto& m : ms) {
        if (!m.valid || m.spec.purpose != TrialPurpose::Scored) continue;
        double d = std::fmod(m.spec.dir_deg, 360.0);
        if (d < 0) d += 360.0;
        const int b = int(std::lround(d / step)) % directions;
        bins[size_t(b)].push_back(&m);
    }
    std::vector<DirectionSummary> out;
    for (int b = 0; b < directions; ++b) {
        DirectionSummary s;
        s.dir_deg = b * step;
        std::vector<double> re, ae, acq, cor, hits, curve, straight;
        for (const auto* m : bins[size_t(b)]) {
            re.push_back(m->radial_error_pct);
            ae.push_back(m->angle_error_deg);
            curve.push_back(m->curve_pct);
            straight.push_back(m->straightness_pct);
            acq.push_back(m->acquisition_ms);
            if (m->hit) cor.push_back(m->correction_ms);
            hits.push_back(m->first_click_hit ? 1.0 : 0.0);
            if (m->flick_class == FlickClass::Undershoot) ++s.n_under;
            else if (m->flick_class == FlickClass::Overshoot) ++s.n_over;
            else ++s.n_on;
        }
        s.n = int(re.size());
        if (s.n > 0) {
            s.mean_radial_error_pct = mean(re);
            s.sd_radial_error_pct = stddev(re);
            s.mean_angle_error_deg = mean(ae);
            s.sd_angle_error_deg = stddev(ae);
            s.median_acquisition_ms = median(acq);
            s.median_correction_ms = cor.empty() ? 0.0 : median(cor);
            s.first_click_hit_rate = mean(hits);
            s.mean_curve_pct = mean(curve);
            s.mean_straightness_pct = mean(straight);
        }
        out.push_back(s);
    }
    return out;
}

OverallSummary summarize_overall(const std::vector<TrialMetrics>& ms) {
    OverallSummary o;
    std::vector<double> react, acq, cor, hits, are, aae, re, ae;
    for (const auto& m : ms) {
        if (m.spec.purpose != TrialPurpose::Scored) continue;
        ++o.n_trials;
        if (!m.valid) { ++o.n_invalid; continue; }
        ++o.n_valid;
        if (!m.hit) ++o.n_timeouts;
        react.push_back(m.reaction_ms);
        acq.push_back(m.acquisition_ms);
        if (m.hit) cor.push_back(m.correction_ms);
        hits.push_back(m.first_click_hit ? 1.0 : 0.0);
        are.push_back(std::fabs(m.radial_error_pct));
        aae.push_back(std::fabs(m.angle_error_deg));
        re.push_back(m.radial_error_pct);
        ae.push_back(m.angle_error_deg);
        if (m.flick_class == FlickClass::Undershoot) ++o.n_under;
        else if (m.flick_class == FlickClass::Overshoot) ++o.n_over;
        else ++o.n_on;
    }
    if (o.n_valid > 0) {
        o.median_reaction_ms = median(react);
        o.median_acquisition_ms = median(acq);
        o.median_correction_ms = cor.empty() ? 0.0 : median(cor);
        o.first_click_hit_rate = mean(hits);
        o.mean_abs_radial_error_pct = mean(are);
        o.mean_abs_angle_error_deg = mean(aae);
        o.sd_radial_error_pct = stddev(re);
        o.sd_angle_error_deg = stddev(ae);
    }
    return o;
}

}  // namespace fit
