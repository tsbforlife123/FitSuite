#include "fit/aim_report.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "fit/stats.hpp"

namespace fit {

const char* task_title(Task t) {
    switch (t) {
        case Task::Flick: return "Flicks";
        case Task::Micro: return "Micro-adjustments";
        case Task::Switch: return "Target switching";
        case Task::Track: return "Tracking";
    }
    return "Flicks";
}

namespace {

const char* kDirNames[8] = {"right", "up-right", "up", "up-left", "left", "down-left", "down", "down-right"};

double wrap_deg(double a) {
    a = std::fmod(a, 360.0);
    return a < 0 ? a + 360.0 : a;
}

double ang_dist_deg(double y1, double p1, double y2, double p2) {
    // Small-angle distance on the view sphere (cos(pitch) shrinks yaw differences away from the horizon).
    const double cp = std::cos(0.5 * (p1 + p2) * kDegToRad);
    return std::hypot((y1 - y2) * cp, p1 - p2);
}

}  // namespace

TrackingResult analyze_tracking(const std::vector<TrackSample>& all, int round) {
    TrackingResult r;
    std::vector<const TrackSample*> s;
    for (const auto& x : all)
        if (round < 0 || x.round == round) s.push_back(&x);
    if (s.size() < 60) {
        r.summary = "Not enough tracking data.";
        return r;
    }
    const int64_t t_first = s.front()->t_us;
    double on = 0.0, err = 0.0, total = 0.0;
    std::array<double, 8> lag_sum{}, lag_w{};
    for (size_t i = 0; i < s.size(); ++i) {
        const TrackSample& a = *s[i];
        const double dt_s = i + 1 < s.size() ? double(s[i + 1]->t_us - a.t_us) / 1e6 : 0.0;
        if (dt_s <= 0.0 || dt_s > 0.1 || s[i + 1]->round != a.round) continue;
        const double e = ang_dist_deg(a.aim_yaw, a.aim_pitch, a.target_yaw, a.target_pitch);
        total += dt_s;
        err += e * dt_s;
        if (e <= a.radius) on += dt_s;

        // Direction gain, from steady stretches only: after the first 0.3 s, target moving steadily
        // for the last 0.25 s, crosshair still near it.
        if (a.t_us - t_first < 300000 || e > 6.0) continue;
        const double vs = std::hypot(double(a.target_vyaw), double(a.target_vpitch));
        if (vs < 5.0) continue;
        size_t j = i;
        while (j > 0 && a.t_us - s[j]->t_us < 250000 && s[j - 1]->round == a.round) --j;
        if (a.t_us - s[j]->t_us < 200000) continue;
        const TrackSample& b = *s[j];
        const double vb = std::hypot(double(b.target_vyaw), double(b.target_vpitch));
        if (vb < 5.0) continue;
        const double turn = std::fabs(std::remainder(std::atan2(a.target_vpitch, a.target_vyaw) -
                                                         std::atan2(b.target_vpitch, b.target_vyaw),
                                                     2.0 * kPi)) * kRadToDeg;
        if (turn > 20.0 || vb / vs > 1.33 || vs / vb > 1.33) continue;
        // Distance behind the target along its path, as time.
        const double cp = std::cos(a.aim_pitch * kDegToRad);
        const double tvy = double(a.target_vyaw) * cp, tvp = double(a.target_vpitch);
        const double tv = std::hypot(tvy, tvp);
        if (tv < 1e-6) continue;
        const double ey = (double(a.target_yaw) - double(a.aim_yaw)) * cp, ep = double(a.target_pitch) - double(a.aim_pitch);
        const double behind_deg = (ey * tvy + ep * tvp) / tv;
        const int bin = int(std::lround(wrap_deg(std::atan2(tvp, tvy) * kRadToDeg) / 45.0)) % 8;
        lag_sum[size_t(bin)] += behind_deg / tv * 1000.0 * dt_s;
        lag_w[size_t(bin)] += dt_s;
        r.lag_n[size_t(bin)]++;
    }
    r.samples = int(s.size());
    if (total <= 0.0) {
        r.summary = "Not enough tracking data.";
        return r;
    }
    r.ok = true;
    r.on_target_pct = 100.0 * on / total;
    r.mean_error_deg = err / total;
    std::vector<double> lags;
    for (int b = 0; b < 8; ++b) {
        r.lag_ms[size_t(b)] = std::nan("");
        if (r.lag_n[size_t(b)] >= 60 && lag_w[size_t(b)] > 0.0) {
            r.lag_ms[size_t(b)] = lag_sum[size_t(b)] / lag_w[size_t(b)];
            lags.push_back(r.lag_ms[size_t(b)]);
        }
    }
    char buf[256];
    std::snprintf(buf, sizeof buf, "On target %.0f%% of the time, %.1f° off on average.", r.on_target_pct, r.mean_error_deg);
    r.summary = buf;
    if (lags.size() >= 5) {
        const double mid = median(lags);
        int worst = -1;
        double dev = 0.0;
        for (int b = 0; b < 8; ++b)
            if (!std::isnan(r.lag_ms[size_t(b)]) && r.lag_ms[size_t(b)] - mid > dev) {
                dev = r.lag_ms[size_t(b)] - mid;
                worst = b;
            }
        if (worst >= 0 && dev >= 30.0) {
            std::snprintf(buf, sizeof buf, " You fall furthest behind targets moving %s (about %.0f ms more than in other directions).",
                          kDirNames[worst], dev);
            r.summary += buf;
        } else {
            r.summary += " You keep up about equally in every direction.";
        }
    }
    return r;
}

AimReport make_aim_report(const Session& s) {
    AimReport rep;
    for (const auto& rr : s.rounds) {
        RoundReport x;
        x.round = rr.index;
        x.task = rr.task;
        x.condition = rr.condition;
        x.duration_s = rr.t_end_us > rr.t_start_us ? double(rr.t_end_us - rr.t_start_us) / 1e6 : 0.0;
        if (rr.task == Task::Track) {
            x.tracking = analyze_tracking(s.tracking, rr.index);
            rep.rounds.push_back(x);
            continue;
        }
        std::vector<double> times;
        for (const auto& t : s.trials) {
            if (t.spec.block != rr.index || t.spec.purpose != TrialPurpose::Scored) continue;
            if (t.outcome == TrialOutcome::Interrupted) continue;
            ++x.targets;
            if (t.outcome == TrialOutcome::Hit) {
                ++x.hits;
                times.push_back(double(t.t_end_us - t.t_appear_us) / 1000.0);
            } else if (t.outcome == TrialOutcome::Miss) {
                ++x.misses;
            } else {
                ++x.timeouts;
            }
        }
        if (rr.task == Task::Switch) x.timeouts += rr.timeouts;  // targets that expired unshot
        x.targets = x.hits + x.misses + x.timeouts;
        x.accuracy_pct = x.targets ? 100.0 * x.hits / x.targets : 0.0;
        x.median_time_ms = times.empty() ? 0.0 : median(times);
        x.kills_per_min = x.duration_s > 0.0 ? x.hits * 60.0 / x.duration_s : 0.0;
        rep.rounds.push_back(x);
    }
    // One line per kind of round (flick rounds pooled).
    for (Task task : {Task::Flick, Task::Micro, Task::Switch, Task::Track}) {
        int targets = 0, hits = 0;
        double secs = 0.0;
        std::vector<double> med;
        const TrackingResult* tr = nullptr;
        for (const auto& x : rep.rounds) {
            if (x.task != task || x.condition != Condition::Baseline) continue;
            targets += x.targets;
            hits += x.hits;
            secs += x.duration_s;
            if (x.median_time_ms > 0) med.push_back(x.median_time_ms);
            if (task == Task::Track && x.tracking.ok) tr = &x.tracking;
        }
        char buf[320];
        if (task == Task::Track) {
            if (tr) rep.lines.push_back(std::string("Tracking: ") + tr->summary);
            continue;
        }
        if (targets == 0) continue;
        if (task == Task::Switch)
            std::snprintf(buf, sizeof buf, "%s: %d of %d targets (%.0f%%), %.0f per minute.", task_title(task), hits, targets,
                          100.0 * hits / targets, secs > 0 ? hits * 60.0 / secs : 0.0);
        else
            std::snprintf(buf, sizeof buf, "%s: %d of %d targets hit (%.0f%%), %.0f ms per target.", task_title(task), hits,
                          targets, 100.0 * hits / targets, med.empty() ? 0.0 : mean(med));
        rep.lines.push_back(buf);
    }
    return rep;
}

}  // namespace fit
