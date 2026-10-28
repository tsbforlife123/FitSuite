#include "fit/motion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "fit/optimize.hpp"
#include "fit/rng.hpp"

namespace fit {

MotionPlan make_motion_plan(int n_configs, const MotionPlanOptions& o) {
    MotionPlan p;
    n_configs = std::max(n_configs, 1);
    Rng rng(o.seed ^ 0x40710Full);
    std::vector<int> order(static_cast<size_t>(n_configs));
    for (int i = 0; i < n_configs; ++i) order[size_t(i)] = i;
    int idx = 0;
    for (int round = 0; round < std::max(o.blocks_per_config, 1); ++round) {
        rng.shuffle(order);
        // Avoid running the same configuration twice in a row across round boundaries.
        if (!p.block_config.empty() && n_configs > 1 && order.front() == p.block_config.back())
            std::swap(order.front(), order.back());
        for (int c : order) {
            std::vector<MotionTrialSpec> block;
            const int b = int(p.blocks.size());
            for (double v : o.speeds_sw_s)
                for (int r = 0; r < std::max(o.reps_per_speed_per_block, 1); ++r) {
                    MotionTrialSpec s;
                    s.index = idx++;
                    s.config = c;
                    s.block = b;
                    s.speed_sw_s = v;
                    s.orientation = int(rng.below(4));
                    block.push_back(s);
                }
            rng.shuffle(block);
            p.blocks.push_back(std::move(block));
            p.block_config.push_back(c);
        }
    }
    return p;
}

namespace {

constexpr double kLapse = 0.02;

double p_correct(double x, double mu, double s) {
    const double z = -(x - mu) / s;
    return 0.25 + 0.75 * (1.0 - kLapse) / (1.0 + std::exp(-z));
}

struct Point {
    double x;
    bool y;
};

bool fit_points(const std::vector<Point>& pts, double& mu, double& s) {
    if (pts.size() < 8) return false;
    double xs = 0.0;
    for (const auto& p : pts) xs += p.x;
    const double x0 = xs / double(pts.size());
    auto nll = [&](const std::vector<double>& u) {
        const double m = u[0];
        const double sl = std::clamp(std::exp(u[1]), 0.05, 3.0);
        double l = 0.0;
        for (const auto& p : pts) {
            const double pc = std::clamp(p_correct(p.x, m, sl), 1e-9, 1.0 - 1e-9);
            l -= p.y ? std::log(pc) : std::log(1.0 - pc);
        }
        // Weak prior keeps the midpoint near the tested range when accuracy is flat.
        l += 0.5 * ((m - x0) / 4.0) * ((m - x0) / 4.0);
        return l;
    };
    const auto r = nelder_mead(nll, {x0, std::log(0.5)}, 0.3, 800);
    mu = r.x[0];
    s = std::clamp(std::exp(r.x[1]), 0.05, 3.0);
    return std::isfinite(mu) && std::isfinite(s);
}

double threshold_from(double mu, double s) {
    // p = 0.75  ->  logistic(z) = 0.5 / (0.75 (1 - lapse))
    const double q = 0.5 / (0.75 * (1.0 - kLapse));
    const double z = std::log(q / (1.0 - q));
    return std::exp2(mu - z * s);
}

std::vector<Point> points_for(const std::vector<MotionResponse>& rs, int config) {
    std::vector<Point> pts;
    for (const auto& r : rs)
        if (r.spec.config == config && r.spec.speed_sw_s > 0) pts.push_back({std::log2(r.spec.speed_sw_s), r.correct});
    return pts;
}

std::vector<Point> resample_by_level(const std::vector<Point>& pts, Rng& rng) {
    std::map<long, std::vector<const Point*>> lv;
    for (const auto& p : pts) lv[std::lround(p.x * 1000)].push_back(&p);
    std::vector<Point> out;
    out.reserve(pts.size());
    for (auto& kv : lv)
        for (size_t i = 0; i < kv.second.size(); ++i) out.push_back(*kv.second[size_t(rng.below(kv.second.size()))]);
    return out;
}

double accuracy_of(const std::vector<Point>& pts) {
    if (pts.empty()) return 0.0;
    double c = 0.0;
    for (const auto& p : pts) c += p.y ? 1.0 : 0.0;
    return c / double(pts.size());
}

}  // namespace

PsychometricFit fit_psychometric(const std::vector<MotionResponse>& responses, int config, int reps, uint64_t seed) {
    PsychometricFit f;
    const auto pts = points_for(responses, config);
    f.n = int(pts.size());
    f.overall_accuracy = accuracy_of(pts);
    std::map<long, std::pair<int, int>> lv;
    for (const auto& p : pts) {
        auto& e = lv[std::lround(p.x * 1000)];
        e.first += p.y ? 1 : 0;
        e.second += 1;
    }
    for (const auto& kv : lv) {
        f.speeds.push_back(std::exp2(double(kv.first) / 1000.0));
        f.accuracy.push_back(double(kv.second.first) / double(kv.second.second));
        f.counts.push_back(kv.second.second);
    }
    if (!fit_points(pts, f.mu, f.slope)) return f;
    f.threshold_sw_s = threshold_from(f.mu, f.slope);
    Rng rng(seed ^ uint64_t(config) * 0x9E37ull);
    std::vector<double> th;
    for (int i = 0; i < reps; ++i) {
        double m, s;
        if (fit_points(resample_by_level(pts, rng), m, s)) th.push_back(std::log2(threshold_from(m, s)));
    }
    if (th.size() > 20) {
        const Interval li = percentile_interval(th);
        f.threshold_ci = {std::exp2(li.lo), std::exp2(li.hi)};
    } else {
        f.threshold_ci = {f.threshold_sw_s, f.threshold_sw_s};
    }
    f.ok = true;
    return f;
}

namespace {
std::string times(double r) {
    if (r >= 10.0) return "more than 10×";
    if (r < 0.1) return "less than 0.1×";
    char b[32];
    std::snprintf(b, sizeof b, "%.1f×", r);
    return b;
}
}  // namespace

MotionComparison compare_motion(const std::vector<MotionResponse>& responses, int a, int b, int reps, uint64_t seed) {
    MotionComparison c;
    const auto pa = points_for(responses, a), pb = points_for(responses, b);
    if (pa.size() < 20 || pb.size() < 20) {
        c.verdict = "insufficient-data";
        c.explanation = "Not enough answers for both configurations (at least 20 each).";
        return c;
    }
    double ma, sa, mb, sb;
    if (!fit_points(pa, ma, sa) || !fit_points(pb, mb, sb)) {
        c.verdict = "insufficient-data";
        c.explanation = "The answers could not be fitted.";
        return c;
    }
    c.ok = true;
    c.threshold_ratio = threshold_from(mb, sb) / threshold_from(ma, sa);
    c.accuracy_diff = accuracy_of(pb) - accuracy_of(pa);
    Rng rng(seed ^ (uint64_t(a) << 8) ^ uint64_t(b));
    std::vector<double> rr, ad;
    for (int i = 0; i < reps; ++i) {
        const auto ra = resample_by_level(pa, rng);
        const auto rb = resample_by_level(pb, rng);
        double m1, s1, m2, s2;
        if (fit_points(ra, m1, s1) && fit_points(rb, m2, s2)) rr.push_back(std::log(threshold_from(m2, s2) / threshold_from(m1, s1)));
        ad.push_back(accuracy_of(rb) - accuracy_of(ra));
    }
    if (rr.size() > 20) {
        const Interval li = percentile_interval(rr);
        c.threshold_ratio_ci = {std::exp(li.lo), std::exp(li.hi)};
    }
    c.accuracy_diff_ci = percentile_interval(ad);
    const std::string ratio = times(c.threshold_ratio), lo = times(c.threshold_ratio_ci.lo), hi = times(c.threshold_ratio_ci.hi);
    if (c.threshold_ratio_ci.lo > 1.0 && c.accuracy_diff_ci.lo > -0.02) {
        c.verdict = "better";
        c.explanation = "Details stayed readable up to about " + ratio + " the speed (95% range " + lo + " to " + hi + ").";
    } else if (c.threshold_ratio_ci.hi < 1.0) {
        c.verdict = "worse";
        c.explanation = "Details became unreadable sooner: at about " + ratio + " the speed (95% range " + lo + " to " + hi + ").";
    } else {
        c.verdict = "no-reliable-difference";
        c.explanation = "No reliable difference in motion clarity (speed ratio " + ratio + ", 95% range " + lo + " to " + hi + ").";
    }
    return c;
}

RefreshAdvice refresh_advice(double current_hz, const std::vector<double>& modes_hz) {
    RefreshAdvice a;
    a.current_hz = current_hz;
    for (double hz : modes_hz)
        if (std::isfinite(hz) && hz > a.max_hz) a.max_hz = hz;
    a.slower = current_hz > 0.0 && a.max_hz - current_hz >= 5.0;
    return a;
}

std::string refresh_dismiss_key(const std::string& monitor_id, double max_hz) {
    return monitor_id + "@" + std::to_string(int(std::lround(max_hz)));
}

}  // namespace fit
