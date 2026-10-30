#include "fit/mouse_fit.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <sstream>

#include "fit/optimize.hpp"
#include "fit/stats.hpp"

namespace fit {

namespace {

struct Regression {
    std::vector<double> beta_ols;
    std::vector<double> beta_ridge;
    Matrix cov_hc1;
    std::vector<double> weights;
    double sigma2 = 0.0;
    bool ok = false;
};

// Huber-weighted least squares; HC1 (heteroscedasticity-robust) covariance for tests; ridge
// shrinkage of the penalised columns (prior sd `tau`) for the estimate actually used.
Regression robust_fit(const Matrix& X, const std::vector<double>& y, const std::vector<bool>& penalized, double tau) {
    Regression R;
    const size_t n = X.rows, p = X.cols;
    if (n <= p + 2) return R;
    std::vector<double> w(n, 1.0);
    std::vector<double> beta(p, 0.0), resid(n, 0.0);
    auto wls = [&](const std::vector<double>& lambda) -> std::optional<std::vector<double>> {
        Matrix A(p, p);
        std::vector<double> b(p, 0.0);
        for (size_t i = 0; i < n; ++i)
            for (size_t r = 0; r < p; ++r) {
                const double xr = X(i, r) * w[i];
                b[r] += xr * y[i];
                for (size_t c = 0; c < p; ++c) A(r, c) += xr * X(i, c);
            }
        for (size_t r = 0; r < p; ++r) A(r, r) += lambda[r];
        return solve(A, b);
    };
    const std::vector<double> zero(p, 0.0);
    for (int it = 0; it < 8; ++it) {
        auto sol = wls(zero);
        if (!sol) return R;
        beta = *sol;
        for (size_t i = 0; i < n; ++i) {
            double f = 0.0;
            for (size_t c = 0; c < p; ++c) f += X(i, c) * beta[c];
            resid[i] = y[i] - f;
        }
        const double s = std::max(mad(resid), 1e-9);
        const double c = 1.345 * s;
        double change = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double nw = std::fabs(resid[i]) <= c ? 1.0 : c / std::fabs(resid[i]);
            change = std::max(change, std::fabs(nw - w[i]));
            w[i] = nw;
        }
        if (change < 1e-4) break;
    }
    // Final OLS with the converged weights.
    auto sol = wls(zero);
    if (!sol) return R;
    beta = *sol;
    double sw = 0.0, swr2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double f = 0.0;
        for (size_t c = 0; c < p; ++c) f += X(i, c) * beta[c];
        resid[i] = y[i] - f;
        sw += w[i];
        swr2 += w[i] * resid[i] * resid[i];
    }
    R.sigma2 = swr2 / std::max(sw - double(p), 1.0);

    // M-estimator sandwich: bread = Σ ψ'(r) x xᵀ (ψ' = 1 inside the Huber band, 0 outside),
    // meat = Σ ψ(r)² x xᵀ with ψ(r) = w·r (the clipped residual). Using the IRLS weights in the
    // bread instead would understate the variance and make the tests too eager.
    const double c_band = 1.345 * std::max(mad(resid), 1e-9);
    Matrix A(p, p), meat(p, p);
    for (size_t i = 0; i < n; ++i) {
        const double psi = w[i] * resid[i];
        const double dpsi = std::fabs(resid[i]) <= c_band ? 1.0 : 0.0;
        for (size_t r = 0; r < p; ++r)
            for (size_t c = 0; c < p; ++c) {
                A(r, c) += dpsi * X(i, r) * X(i, c);
                meat(r, c) += psi * psi * X(i, r) * X(i, c);
            }
    }
    auto bread = inverse(A);
    if (!bread) return R;
    Matrix cov = multiply(multiply(*bread, meat), *bread);
    const double scale = double(n) / double(n - p);
    for (double& v : cov.a) v *= scale;

    std::vector<double> lambda(p, 0.0);
    for (size_t c = 0; c < p; ++c)
        if (penalized[c]) lambda[c] = R.sigma2 / (tau * tau);
    auto ridge = wls(lambda);
    if (!ridge) return R;

    R.beta_ols = beta;
    R.beta_ridge = *ridge;
    R.cov_hc1 = cov;
    R.weights = w;
    R.ok = true;
    return R;
}

// Wald test referred to an F(q, n - p) distribution, which is less eager than χ²(q) in the
// sample sizes of a calibration session (about 100 flicks).
ComponentTest wald(const Regression& R, const std::vector<size_t>& cols, double alpha, size_t n, size_t p) {
    ComponentTest t;
    const size_t q = cols.size();
    t.df = double(q);
    Matrix V(q, q);
    std::vector<double> b(q);
    for (size_t i = 0; i < q; ++i) {
        b[i] = R.beta_ols[cols[i]];
        for (size_t j = 0; j < q; ++j) V(i, j) = R.cov_hc1(cols[i], cols[j]);
    }
    auto Vi = inverse(V);
    if (!Vi) return t;
    double w = 0.0;
    for (size_t i = 0; i < q; ++i)
        for (size_t j = 0; j < q; ++j) w += b[i] * (*Vi)(i, j) * b[j];
    t.statistic = w;
    t.p_value = f_sf(w / double(q), double(q), std::max(double(n) - double(p), 1.0));
    t.significant = t.p_value < alpha;
    return t;
}

int dir_bin(double dir_deg, int bins = 8) {
    double d = std::fmod(dir_deg, 360.0);
    if (d < 0) d += 360.0;
    return int(std::lround(d / (360.0 / bins))) % bins;
}

struct Obs {
    const TrialMetrics* m;
    int dist_level;
    double theta;   // target direction, screen convention (rad)
    Vec2 e;         // endpoint / distance (raw coords)
    Vec2 t;         // unit target direction (raw coords)
    double w = 1.0; // robust weight
};

// Largest per-direction bias. Radial bias is relative to the per-distance average (the player's
// normal under/overshoot is kept on purpose); angle bias is absolute.
void direction_biases(const std::vector<Obs>& obs, int n_levels, const std::function<Vec2(const Obs&)>& endpoint,
                      double& max_radial_pct, double& max_angle_deg) {
    std::vector<double> sum_ratio(size_t(n_levels), 0.0), cnt(size_t(n_levels), 0.0);
    std::vector<Vec2> ends(obs.size());
    for (size_t i = 0; i < obs.size(); ++i) {
        ends[i] = endpoint(obs[i]);
        sum_ratio[size_t(obs[i].dist_level)] += dot(ends[i], obs[i].t);
        cnt[size_t(obs[i].dist_level)] += 1.0;
    }
    std::vector<double> rel_sum(8, 0.0), ang_sum(8, 0.0), n(8, 0.0);
    for (size_t i = 0; i < obs.size(); ++i) {
        const double c = sum_ratio[size_t(obs[i].dist_level)] / std::max(cnt[size_t(obs[i].dist_level)], 1.0);
        const int b = dir_bin(obs[i].m->spec.dir_deg);
        rel_sum[size_t(b)] += dot(ends[i], obs[i].t) / c - 1.0;
        ang_sum[size_t(b)] += signed_screen_angle_between(obs[i].t, ends[i]);
        n[size_t(b)] += 1.0;
    }
    max_radial_pct = 0.0;
    max_angle_deg = 0.0;
    for (int b = 0; b < 8; ++b) {
        if (n[size_t(b)] == 0) continue;
        max_radial_pct = std::max(max_radial_pct, std::fabs(rel_sum[size_t(b)] / n[size_t(b)]) * 100.0);
        max_angle_deg = std::max(max_angle_deg, std::fabs(ang_sum[size_t(b)] / n[size_t(b)]) * kRadToDeg);
    }
}

}  // namespace

namespace {
// Minimum-jerk speed profile, normalised to a peak of 1: v(τ) = 30τ²(1−τ)² / 1.875.
double min_jerk_speed(double tau) { return 30.0 * tau * tau * (1.0 - tau) * (1.0 - tau) / 1.875; }
}  // namespace

double speed_curve_flick_gain(const SpeedCurve& c, double peak) {
    if (c.empty() || !(peak > 0.0)) return 1.0;
    double num = 0.0, den = 0.0;
    for (int i = 1; i < 400; ++i) {
        const double v = min_jerk_speed(i / 400.0) * peak;
        num += v * c.scale(v);
        den += v;
    }
    return den > 0.0 ? num / den : 1.0;
}

SpeedCurve make_speed_curve(double k, double v_mid, double v_slow, double v_fast) {
    SpeedCurve c;
    if (!(v_mid > 0.0) || !(v_slow > 0.0) || !(v_fast > v_slow) || k == 0.0) return c;
    const double cap = kSpeedCurveMaxDeviation;
    auto build = [&](double v_ref) {
        SpeedCurve out;
        const double x0 = v_slow * 0.1, x1 = v_fast * 1.5;
        const int n = 24;
        for (int i = 0; i < n; ++i) {
            const double x = x0 * std::pow(x1 / x0, double(i) / double(n - 1));
            const double y = std::clamp(std::pow(x / v_ref, k), 1.0 - cap, 1.0 + cap);
            out.points.push_back({double(float(x)), double(float(y))});
        }
        // Raw Accel extends the last segment beyond the last point: end flat.
        out.points.push_back({double(float(x1 * 2.0)), out.points.back().second});
        return out;
    };
    // Choose the reference speed so a flick peaking at v_mid keeps its distance (a few bisection
    // steps on log v_ref; the flick gain falls as v_ref rises when k > 0, and rises when k < 0).
    double lo = std::log(v_mid) - 3.0, hi = std::log(v_mid) + 3.0;
    for (int it = 0; it < 60; ++it) {
        const double mid = 0.5 * (lo + hi);
        const double g = speed_curve_flick_gain(build(std::exp(mid)), v_mid);
        if ((g > 1.0) == (k > 0.0)) lo = mid; else hi = mid;
    }
    c = build(std::exp(0.5 * (lo + hi)));
    return c;
}

FitResult fit_profiles(const Session& s, const FitOptions& opt) {
    FitResult r;
    if (s.kind != SessionKind::Calibration) {
        r.message = "Profiles can only be fitted from a calibration session. Validation trials are never used for fitting.";
        return r;
    }
    const GameMapping& map = s.conditions.mapping;
    if (!map.valid()) { r.message = "The session has no valid sensitivity mapping."; return r; }

    std::vector<TrialMetrics> metrics;
    for (const auto& t : s.trials) {
        if (t.spec.purpose != TrialPurpose::Scored) continue;
        if (t.spec.condition != Condition::Baseline) {
            r.message = "Calibration data must be recorded with baseline (uncorrected) input.";
            return r;
        }
        metrics.push_back(analyze_trial(t, map, opt.segment));
    }
    r.n_trials = int(metrics.size());
    r.by_direction = summarize_by_direction(metrics, 8);
    r.overall = summarize_overall(metrics);

    // ---- select usable flicks -----------------------------------------------------------------
    // Distance level: the planned distance, per kind of round (a micro-adjustment at 4° is not the
    // same movement as a flick at 4°). Switching flicks are grouped by distance band.
    auto level_key = [](const TargetSpec& s) -> long {
        double d = s.level_deg();
        if (s.task == Task::Switch) d = d < 5.0 ? 3.0 : d < 10.0 ? 7.0 : d < 16.0 ? 13.0 : 22.0;
        return long(s.task) * 100000L + std::lround(d * 100.0);
    };
    std::map<long, int> level_of;  // task + distance (0.01° units) -> level index
    std::vector<const TrialMetrics*> use;
    for (const auto& m : metrics) {
        if (!m.valid) { ++r.n_invalid; continue; }
        if (std::fabs(m.radial_ratio - 1.0) > opt.gross_radial_error ||
            std::fabs(m.angle_error_deg) > opt.gross_angle_error_deg) {
            ++r.n_excluded_gross;
            continue;
        }
        use.push_back(&m);
    }
    // Each level needs a handful of flicks to estimate its own average under/overshoot; the few
    // flicks of a rare level (a switching distance band hardly used) are left out.
    {
        std::map<long, int> count;
        for (const auto* m : use) ++count[level_key(m->spec)];
        std::vector<const TrialMetrics*> kept;
        for (const auto* m : use)
            if (count[level_key(m->spec)] >= 5) kept.push_back(m);
            else ++r.n_excluded_gross;
        use.swap(kept);
        for (const auto* m : use) level_of.emplace(level_key(m->spec), 0);
    }
    int li = 0;
    for (auto& kv : level_of) { kv.second = li++; r.distances_deg.push_back(double(kv.first % 100000L) / 100.0); }
    const int n_levels = int(level_of.size());
    r.n_used = int(use.size());

    std::vector<int> per_dir(8, 0);
    for (const auto* m : use) ++per_dir[size_t(dir_bin(m->spec.dir_deg))];
    static const char* names[8] = {"right", "up-right", "up", "up-left", "left", "down-left", "down", "down-right"};
    for (int b = 0; b < 8; ++b)
        if (per_dir[size_t(b)] < opt.min_per_direction) {
            std::ostringstream os;
            os << names[b] << ": " << per_dir[size_t(b)] << " usable flicks (need " << opt.min_per_direction << ")";
            r.data_problems.push_back(os.str());
        }
    if (r.n_used < opt.min_total) {
        std::ostringstream os;
        os << r.n_used << " usable flicks in total (need " << opt.min_total << ")";
        r.data_problems.push_back(os.str());
    }
    if (n_levels < opt.min_distances) r.data_problems.push_back("flicks at fewer than two target distances");
    if (!r.data_problems.empty()) {
        r.message = "Not enough calibration data to recommend a profile yet. Run more calibration blocks.";
        return r;
    }

    std::vector<Obs> obs;
    obs.reserve(use.size());
    for (const auto* m : use) {
        Obs o;
        o.m = m;
        o.dist_level = level_of[level_key(m->spec)];
        o.theta = m->target_angle_rad;
        o.e = m->endpoint_counts / m->target_dist_counts;
        o.t = raw_unit_from_screen_angle(m->target_angle_rad);
        obs.push_back(o);
    }
    const size_t n = obs.size();

    // ---- smooth directional model ---------------------------------------------------------------
    // log radial ratio = c_distance + Σ Fourier(θ)      (gain)
    // angle error      = c0 + Σ Fourier(θ)              (angle)
    const size_t pg = size_t(n_levels) + 4;
    Matrix Xg(n, pg), Xa(n, 5);
    std::vector<double> yg(n), ya(n);
    for (size_t i = 0; i < n; ++i) {
        const double th = obs[i].theta;
        Xg(i, size_t(obs[i].dist_level)) = 1.0;
        Xg(i, size_t(n_levels) + 0) = std::cos(th);
        Xg(i, size_t(n_levels) + 1) = std::sin(th);
        Xg(i, size_t(n_levels) + 2) = std::cos(2 * th);
        Xg(i, size_t(n_levels) + 3) = std::sin(2 * th);
        yg[i] = std::log(obs[i].m->radial_ratio);
        Xa(i, 0) = 1.0;
        Xa(i, 1) = std::cos(th);
        Xa(i, 2) = std::sin(th);
        Xa(i, 3) = std::cos(2 * th);
        Xa(i, 4) = std::sin(2 * th);
        ya[i] = obs[i].m->angle_error_deg * kDegToRad;
    }
    std::vector<bool> pen_g(pg, false), pen_a(5, false);
    for (size_t c = size_t(n_levels); c < pg; ++c) pen_g[c] = true;
    for (size_t c = 1; c < 5; ++c) pen_a[c] = true;
    const Regression Rg = robust_fit(Xg, yg, pen_g, opt.prior_sd_log_gain);
    const Regression Ra = robust_fit(Xa, ya, pen_a, opt.prior_sd_angle_deg * kDegToRad);
    if (!Rg.ok || !Ra.ok) {
        r.message = "The calibration data could not be fitted (degenerate directions). Run another calibration.";
        return r;
    }
    std::vector<size_t> fg;
    for (size_t c = size_t(n_levels); c < pg; ++c) fg.push_back(c);
    r.gain_test = wald(Rg, fg, opt.alpha, n, pg);
    r.angle_test = wald(Ra, {0, 1, 2, 3, 4}, opt.alpha, n, 5);
    for (size_t i = 0; i < n; ++i) obs[i].w = std::min(Rg.weights[i], Ra.weights[i]);

    DirectionalProfile d;
    if (r.gain_test.significant) {
        d.gain_cos[0] = -Rg.beta_ridge[size_t(n_levels) + 0];
        d.gain_sin[0] = -Rg.beta_ridge[size_t(n_levels) + 1];
        d.gain_cos[1] = -Rg.beta_ridge[size_t(n_levels) + 2];
        d.gain_sin[1] = -Rg.beta_ridge[size_t(n_levels) + 3];
    }
    if (r.angle_test.significant) {
        d.angle_const = -Ra.beta_ridge[0];
        d.angle_cos[0] = -Ra.beta_ridge[1];
        d.angle_sin[0] = -Ra.beta_ridge[2];
        d.angle_cos[1] = -Ra.beta_ridge[3];
        d.angle_sin[1] = -Ra.beta_ridge[4];
    }
    r.directional_bounds_scaled = enforce_bounds(d, opt.bounds);
    const bool dir_material =
        d.max_gain_deviation() >= opt.negligible_gain || d.max_angle_deg() >= opt.negligible_angle_deg;
    if (!dir_material) d = DirectionalProfile{};
    r.directional = d;
    r.directional_recommended = (r.gain_test.significant || r.angle_test.significant) && dir_material;

    // ---- Raw Accel-compatible model -------------------------------------------------------------
    const double gb = std::log(1.0 + opt.bounds.max_gain_deviation);
    const double rb = opt.bounds.max_angle_deg;
    const bool asym = opt.raw_accel_asymmetry;
    auto make_ra = [&](const std::vector<double>& u) {
        RawAccelProfile ra;
        ra.rotation_deg = rb * std::tanh(u[0]);
        ra.yx_ratio = std::exp(gb * std::tanh(u[1]));
        if (asym) {
            ra.lr_ratio = std::exp(gb * std::tanh(u[2]));
            ra.ud_ratio = std::exp(gb * std::tanh(u[3]));
        }
        return ra;
    };
    // Model the *measured* endpoint as a function of the exact target: e ≈ c_d · RA⁻¹(t). Putting
    // the noisy endpoint on the left avoids the errors-in-variables bias that fitting RA(e) ≈ c_d t
    // would have (it shrinks the Y/X ratio toward the noise level). c_d (the player's normal
    // under/overshoot per distance) is profiled out. Gross errors were already excluded.
    auto residuals = [&](const RawAccelProfile& ra, std::vector<Vec2>& res) {
        std::vector<double> num(size_t(n_levels), 0.0), den(size_t(n_levels), 0.0);
        std::vector<Vec2> m(n);
        for (size_t i = 0; i < n; ++i) {
            m[i] = ra.inverse_apply(obs[i].t);
            num[size_t(obs[i].dist_level)] += dot(obs[i].e, m[i]);
            den[size_t(obs[i].dist_level)] += dot(m[i], m[i]);
        }
        res.resize(n);
        double s2 = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double c = num[size_t(obs[i].dist_level)] / std::max(den[size_t(obs[i].dist_level)], 1e-12);
            res[i] = obs[i].e - m[i] * c;
            s2 += dot(res[i], res[i]);
        }
        return s2;
    };
    std::vector<Vec2> scratch;
    auto rss = [&](const RawAccelProfile& ra) { return residuals(ra, scratch); };
    const size_t q = asym ? 4 : 2;
    auto nm = nelder_mead([&](const std::vector<double>& u) { return rss(make_ra(u)); }, std::vector<double>(q, 0.0), 0.2, 4000);
    RawAccelProfile ra = make_ra(nm.x);

    // Wald test of "no Raw Accel correction" with a trial-clustered sandwich covariance (radial and
    // tangential errors have different spreads, so a pooled F-test would be too eager).
    {
        auto from_theta = [&](const std::vector<double>& th) {
            RawAccelProfile x;
            x.rotation_deg = th[0];
            x.yx_ratio = std::exp(th[1]);
            if (asym) {
                x.lr_ratio = std::exp(th[2]);
                x.ud_ratio = std::exp(th[3]);
            }
            return x;
        };
        std::vector<double> th = {ra.rotation_deg, std::log(ra.yx_ratio)};
        if (asym) {
            th.push_back(std::log(ra.lr_ratio));
            th.push_back(std::log(ra.ud_ratio));
        }
        std::vector<Vec2> r0;
        residuals(from_theta(th), r0);
        std::vector<std::vector<Vec2>> J(q);
        for (size_t j = 0; j < q; ++j) {
            const double h = j == 0 ? 1e-3 : 1e-5;
            auto tp = th, tm = th;
            tp[j] += h;
            tm[j] -= h;
            std::vector<Vec2> rp, rm;
            residuals(from_theta(tp), rp);
            residuals(from_theta(tm), rm);
            J[j].resize(n);
            for (size_t i = 0; i < n; ++i) J[j][i] = (rp[i] - rm[i]) / (2.0 * h);
        }
        Matrix A(q, q), B(q, q);
        for (size_t i = 0; i < n; ++i) {
            std::vector<double> g(q);
            for (size_t a = 0; a < q; ++a) g[a] = dot(J[a][i], r0[i]);
            for (size_t a = 0; a < q; ++a)
                for (size_t b = 0; b < q; ++b) {
                    A(a, b) += dot(J[a][i], J[b][i]);
                    B(a, b) += g[a] * g[b];
                }
        }
        auto Ai = inverse(A);
        if (Ai) {
            Matrix V = multiply(multiply(*Ai, B), *Ai);
            const double scale = double(n) / std::max(double(n) - double(q) - double(n_levels), 1.0);
            for (double& v : V.a) v *= scale;
            auto Vi = inverse(V);
            if (Vi) {
                double w = 0.0;
                for (size_t a = 0; a < q; ++a)
                    for (size_t b = 0; b < q; ++b) w += th[a] * (*Vi)(a, b) * th[b];
                r.raw_accel_test.statistic = w;
                r.raw_accel_test.df = double(q);
                r.raw_accel_test.p_value =
                    f_sf(w / double(q), double(q), std::max(double(n) - double(q) - double(n_levels), 1.0));
                r.raw_accel_test.significant = r.raw_accel_test.p_value < opt.alpha;
            }
        }
    }
    // Keep the player's average sensitivity (mean radial gain over directions = 1) and respect the
    // same per-direction bounds as the smooth profile. With separate up/down/left/right multipliers
    // two of them can stack in one direction, so the bound is checked on the resulting gain and
    // angle in every direction and the whole correction is scaled toward identity if needed.
    {
        Bounds b = opt.bounds;
        b.max_gain_deviation = std::clamp(b.max_gain_deviation, 0.0, kHardMaxGainDeviation);
        b.max_angle_deg = std::clamp(b.max_angle_deg, 0.0, kHardMaxAngleDeg);
        const RawAccelProfile fitted = ra;
        auto scaled = [&](double k) {
            RawAccelProfile x;
            x.rotation_deg = fitted.rotation_deg * k;
            x.yx_ratio = std::exp(k * std::log(fitted.yx_ratio));
            x.lr_ratio = std::exp(k * std::log(fitted.lr_ratio));
            x.ud_ratio = std::exp(k * std::log(fitted.ud_ratio));
            double g = 0.0;
            for (int i = 0; i < 360; ++i) g += x.gain((i + 0.5) * kDegToRad);
            x.sens = 360.0 / g;
            return x;
        };
        auto within = [&](const RawAccelProfile& x) {
            double gm = 0.0, am = 0.0;
            for (int i = 0; i < 360; ++i) {
                const double th = (i + 0.5) * kDegToRad;
                gm = std::max(gm, std::fabs(x.gain(th) - 1.0));
                am = std::max(am, std::fabs(x.angle(th)) * kRadToDeg);
            }
            return gm <= b.max_gain_deviation + 1e-9 && am <= b.max_angle_deg + 1e-9;
        };
        ra = scaled(1.0);
        if (!within(ra)) {
            double lo = 0.0, hi = 1.0;
            for (int it = 0; it < 50; ++it) {
                const double mid = 0.5 * (lo + hi);
                if (within(scaled(mid))) lo = mid; else hi = mid;
            }
            ra = scaled(lo);
            r.raw_accel_bounds_scaled = true;
        }
    }
    double ra_max_gain = 0.0;
    for (int i = 0; i < 360; ++i) ra_max_gain = std::max(ra_max_gain, std::fabs(ra.gain((i + 0.5) * kDegToRad) - 1.0));
    const bool ra_material = ra_max_gain >= opt.negligible_gain || std::fabs(ra.rotation_deg) >= opt.negligible_angle_deg;
    if (!r.raw_accel_test.significant || !ra_material) ra = RawAccelProfile{};
    r.raw_accel = ra;
    r.raw_accel_recommended = r.raw_accel_test.significant && ra_material;
    for (int i = 0; i < 720; ++i) {
        const double th = -kPi + (i + 0.5) * kPi / 360.0;
        r.raw_accel_gain_mismatch = std::max(r.raw_accel_gain_mismatch, std::fabs(ra.gain(th) - r.directional.gain(th)));
        r.raw_accel_angle_mismatch_deg =
            std::max(r.raw_accel_angle_mismatch_deg, std::fabs(ra.angle(th) - r.directional.angle(th)) * kRadToDeg);
    }

    // ---- speed curve ----------------------------------------------------------------------------
    // Players often land long on near targets and short on far ones. Near targets get slow flicks
    // and far ones fast flicks, so a sensitivity that rises (or falls) a little with hand speed
    // evens that out; Raw Accel can apply it as a lookup table. The predictor is the typical peak
    // speed of the flick's distance level, never the flick's own speed: a flick that overshoots is
    // also faster, so its own speed would show a link that is only its error.
    if (opt.speed_curve) {
        std::map<int, std::vector<double>> peaks;  // flick level -> peak hand speeds (counts/ms)
        for (const auto& o : obs)
            if (o.m->spec.task == Task::Flick) peaks[o.dist_level].push_back(o.m->peak_speed_deg_s / map.deg_per_count() / 1000.0);
        std::map<int, double> lv;  // log typical peak speed per level
        for (auto& [l, v] : peaks)
            if (v.size() >= 5) lv[l] = std::log(median(v));
        if (lv.size() >= 3) {
            double mean_lv = 0.0, lmin = 1e9, lmax = -1e9;
            for (const auto& [l, v] : lv) {
                mean_lv += v;
                lmin = std::min(lmin, v);
                lmax = std::max(lmax, v);
            }
            mean_lv /= double(lv.size());
            std::vector<const Obs*> fo;
            for (const auto& o : obs)
                if (o.m->spec.task == Task::Flick && lv.count(o.dist_level)) fo.push_back(&o);
            Matrix Xs(fo.size(), 6);
            std::vector<double> ys(fo.size());
            for (size_t i = 0; i < fo.size(); ++i) {
                const double th = fo[i]->theta;
                Xs(i, 0) = 1.0;
                Xs(i, 1) = lv[fo[i]->dist_level] - mean_lv;
                Xs(i, 2) = std::cos(th);
                Xs(i, 3) = std::sin(th);
                Xs(i, 4) = std::cos(2 * th);
                Xs(i, 5) = std::sin(2 * th);
                ys[i] = std::log(fo[i]->m->radial_ratio);
            }
            const Regression Rs = robust_fit(Xs, ys, std::vector<bool>(6, false), 1.0);
            if (Rs.ok && lmax - lmin > 0.05) {
                r.speed_test = wald(Rs, {1}, opt.alpha, fo.size(), 6);
                double k = -Rs.beta_ols[1];  // landing ∝ speed^β, so sensitivity ∝ speed^−β evens it out
                const double span = std::max(lmax - mean_lv, mean_lv - lmin);
                const double kmax = std::log(1.0 + opt.speed_max_effect) / std::max(span, 1e-6);
                if (std::fabs(k) > kmax) {
                    k = std::copysign(kmax, k);
                    r.speed_bounds_scaled = true;
                }
                r.speed_exponent = k;
                r.speed_slow_cpms = std::exp(lmin);
                r.speed_fast_cpms = std::exp(lmax);
                r.speed_slow_pct = (std::exp(k * (lmin - mean_lv)) - 1.0) * 100.0;
                r.speed_fast_pct = (std::exp(k * (lmax - mean_lv)) - 1.0) * 100.0;
                // In-sample: each level's average landing relative to the average level, before and after.
                std::map<int, std::pair<double, int>> sum;
                for (size_t i = 0; i < fo.size(); ++i) {
                    auto& s = sum[fo[i]->dist_level];
                    s.first += ys[i];
                    s.second += 1;
                }
                double gm = 0.0;
                for (const auto& [l, s] : sum) gm += s.first / s.second;
                gm /= double(sum.size());
                for (const auto& [l, s] : sum) {
                    const double before = s.first / s.second - gm;
                    const double after = before + k * (lv[l] - mean_lv);
                    r.speed_level_bias_before_pct = std::max(r.speed_level_bias_before_pct, std::fabs(std::exp(before) - 1.0) * 100.0);
                    r.speed_level_bias_after_pct = std::max(r.speed_level_bias_after_pct, std::fabs(std::exp(after) - 1.0) * 100.0);
                }
                const double change = std::exp(std::fabs(k) * (lmax - lmin)) - 1.0;
                if (r.speed_test.significant && change >= opt.negligible_speed && std::fabs(k) > 1e-6) {
                    r.raw_accel.speed = make_speed_curve(k, std::exp(mean_lv), std::exp(lmin), std::exp(lmax));
                    r.speed_recommended = true;
                    r.raw_accel_recommended = true;
                }
            }
        }
    }

    // ---- overall speed ----------------------------------------------------------------------------
    // Where first movements land on average, every distance level weighted alike. Short everywhere: a
    // faster overall speed; long everywhere: slower; at most opt.max_overall either way. Most players'
    // first movement stops a little short, so this usually comes out slightly faster; the blind test
    // decides whether the fix is used at all.
    if (opt.overall_speed && n > size_t(n_levels) + 1) {
        std::vector<double> sum(size_t(n_levels), 0.0), cnt(size_t(n_levels), 0.0);
        for (size_t i = 0; i < n; ++i) {
            sum[size_t(obs[i].dist_level)] += yg[i];
            cnt[size_t(obs[i].dist_level)] += 1.0;
        }
        double m = 0.0, used = 0.0, ss = 0.0;
        for (size_t l = 0; l < sum.size(); ++l)
            if (cnt[l] > 0.0) {
                m += sum[l] / cnt[l];
                used += 1.0;
            }
        m /= std::max(used, 1.0);
        for (size_t i = 0; i < n; ++i) {
            const size_t l = size_t(obs[i].dist_level);
            const double d = yg[i] - sum[l] / cnt[l];
            ss += d * d;
        }
        const double df = std::max(double(n) - used, 1.0);
        const double se = std::sqrt(ss / df / double(n));
        const double tt = se > 0.0 ? (m / se) * (m / se) : 0.0;
        r.overall_test.statistic = tt;
        r.overall_test.df = df;
        r.overall_test.p_value = f_sf(tt, 1.0, df);
        r.overall_test.significant = r.overall_test.p_value < opt.alpha;
        r.overall_landing_pct = (std::exp(m) - 1.0) * 100.0;
        const double lim = std::log(1.0 + std::clamp(opt.max_overall, 0.0, kHardMaxGainDeviation));
        const double corr = std::clamp(-m, -lim, lim);
        r.overall_recommended = r.overall_test.significant && std::fabs(std::exp(corr) - 1.0) >= opt.negligible_overall;
        if (r.overall_recommended) {
            r.overall_pct = (std::exp(corr) - 1.0) * 100.0;
            r.raw_accel.sens *= std::exp(corr);
        }
    }

    // ---- in-sample effect (no adaptation assumed) -----------------------------------------------
    direction_biases(obs, n_levels, [](const Obs& o) { return o.e; }, r.max_radial_bias_before_pct,
                     r.max_angle_bias_before_deg);
    direction_biases(obs, n_levels, [&](const Obs& o) { return r.directional.apply(o.e, screen_angle(o.e)); },
                     r.max_radial_bias_after_dir_pct, r.max_angle_bias_after_dir_deg);
    direction_biases(obs, n_levels, [&](const Obs& o) { return r.raw_accel.apply(o.e); }, r.max_radial_bias_after_ra_pct,
                     r.max_angle_bias_after_ra_deg);

    r.ok = true;
    std::ostringstream os;
    if (!r.directional_recommended && !r.fix_recommended()) {
        os << "No correction recommended: your direction-to-direction differences are within measurement noise "
              "or too small to matter. Keep your current setup.";
    } else {
        if (r.overall_recommended)
            os << "Your first movements land " << std::lround(std::fabs(r.overall_landing_pct) * 10) / 10.0 << "% "
               << (r.overall_landing_pct < 0 ? "short" : "long") << " on average, so the fix also makes your overall speed "
               << (r.overall_pct > 0 ? "+" : "") << std::lround(r.overall_pct * 10) / 10.0 << "%. ";
        if (r.directional_recommended || r.raw_accel_test.significant)
            os << "Directional bias found (largest " << std::lround(r.max_radial_bias_before_pct * 10) / 10.0
               << "% distance, " << std::lround(r.max_angle_bias_before_deg * 10) / 10.0 << "° angle). ";
        if (r.speed_recommended)
            os << "Your near and far flicks land differently (up to " << std::lround(r.speed_level_bias_before_pct * 10) / 10.0
               << "%), so the fix also changes sensitivity slightly with hand speed. ";
        os << "A candidate correction is ready; it is not recommended until a blinded validation confirms it.";
    }
    r.message = os.str();
    return r;
}

}  // namespace fit
