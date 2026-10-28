#include "fit/stats.hpp"
#include "fit/common.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace fit {

double mean(const std::vector<double>& v) {
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    // Kahan-compensated sum keeps long frame-time series exact enough.
    double s = 0.0, c = 0.0;
    for (double x : v) {
        const double y = x - c;
        const double t = s + y;
        c = (t - s) - y;
        s = t;
    }
    return s / double(v.size());
}

double variance(const std::vector<double>& v) {
    if (v.size() < 2) return 0.0;
    const double m = mean(v);
    double s = 0.0;
    for (double x : v) s += (x - m) * (x - m);
    return s / double(v.size() - 1);
}

double stddev(const std::vector<double>& v) { return std::sqrt(variance(v)); }

double percentile_sorted(const std::vector<double>& s, double p) {
    if (s.empty()) return std::numeric_limits<double>::quiet_NaN();
    if (s.size() == 1) return s[0];
    p = std::clamp(p, 0.0, 100.0);
    const double h = (double(s.size()) - 1.0) * p / 100.0;
    const size_t lo = size_t(std::floor(h));
    const size_t hi = std::min(lo + 1, s.size() - 1);
    return s[lo] + (h - double(lo)) * (s[hi] - s[lo]);
}

double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return percentile_sorted(v, p);
}

double median(std::vector<double> v) { return percentile(std::move(v), 50.0); }

double mad(const std::vector<double>& v) {
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    const double m = median(v);
    std::vector<double> d;
    d.reserve(v.size());
    for (double x : v) d.push_back(std::fabs(x - m));
    return 1.4826 * median(std::move(d));
}

Interval percentile_interval(std::vector<double> draws, double level) {
    std::sort(draws.begin(), draws.end());
    const double tail = (1.0 - level) / 2.0 * 100.0;
    return {percentile_sorted(draws, tail), percentile_sorted(draws, 100.0 - tail)};
}

// ---- distributions -------------------------------------------------------------------------

double normal_cdf(double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }

double normal_quantile(double p) {
    if (p <= 0.0) return -std::numeric_limits<double>::infinity();
    if (p >= 1.0) return std::numeric_limits<double>::infinity();
    // Acklam's rational approximation, then one Halley refinement step.
    static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                               1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                               6.680131188771972e+01, -1.328068155288572e+01};
    static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                               -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
    static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                               3.754408661907416e+00};
    const double plow = 0.02425, phigh = 1 - plow;
    double x;
    if (p < plow) {
        const double q = std::sqrt(-2 * std::log(p));
        x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    } else if (p <= phigh) {
        const double q = p - 0.5, r = q * q;
        x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
            (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
    } else {
        const double q = std::sqrt(-2 * std::log(1 - p));
        x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    }
    const double e = normal_cdf(x) - p;
    const double u = e * std::sqrt(2 * kPi) * std::exp(x * x / 2);
    return x - u / (1 + x * u / 2);
}

namespace {

double beta_cf(double a, double b, double x) {
    const int kMaxIt = 300;
    const double kEps = 3e-16, kFpMin = 1e-300;
    const double qab = a + b, qap = a + 1.0, qam = a - 1.0;
    double c = 1.0, d = 1.0 - qab * x / qap;
    if (std::fabs(d) < kFpMin) d = kFpMin;
    d = 1.0 / d;
    double h = d;
    for (int m = 1; m <= kMaxIt; ++m) {
        const int m2 = 2 * m;
        double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if (std::fabs(d) < kFpMin) d = kFpMin;
        c = 1.0 + aa / c;
        if (std::fabs(c) < kFpMin) c = kFpMin;
        d = 1.0 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if (std::fabs(d) < kFpMin) d = kFpMin;
        c = 1.0 + aa / c;
        if (std::fabs(c) < kFpMin) c = kFpMin;
        d = 1.0 / d;
        const double del = d * c;
        h *= del;
        if (std::fabs(del - 1.0) < kEps) break;
    }
    return h;
}

}  // namespace

double incomplete_beta(double a, double b, double x) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    const double bt = std::exp(std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b) + a * std::log(x) +
                               b * std::log(1.0 - x));
    if (x < (a + 1.0) / (a + b + 2.0)) return bt * beta_cf(a, b, x) / a;
    return 1.0 - bt * beta_cf(b, a, 1.0 - x) / b;
}

double gamma_p(double a, double x) {
    if (x <= 0.0) return 0.0;
    if (x < a + 1.0) {
        double ap = a, sum = 1.0 / a, del = sum;
        for (int n = 0; n < 500; ++n) {
            ap += 1.0;
            del *= x / ap;
            sum += del;
            if (std::fabs(del) < std::fabs(sum) * 1e-16) break;
        }
        return sum * std::exp(-x + a * std::log(x) - std::lgamma(a));
    }
    return 1.0 - gamma_q(a, x);
}

double gamma_q(double a, double x) {
    if (x <= 0.0) return 1.0;
    if (x < a + 1.0) return 1.0 - gamma_p(a, x);
    const double kFpMin = 1e-300;
    double b = x + 1.0 - a, c = 1.0 / kFpMin, d = 1.0 / b, h = d;
    for (int i = 1; i < 500; ++i) {
        const double an = -i * (i - a);
        b += 2.0;
        d = an * d + b;
        if (std::fabs(d) < kFpMin) d = kFpMin;
        c = b + an / c;
        if (std::fabs(c) < kFpMin) c = kFpMin;
        d = 1.0 / d;
        const double del = d * c;
        h *= del;
        if (std::fabs(del - 1.0) < 1e-16) break;
    }
    return std::exp(-x + a * std::log(x) - std::lgamma(a)) * h;
}

double student_t_cdf(double t, double df) {
    if (!std::isfinite(t)) return t > 0 ? 1.0 : 0.0;
    const double x = df / (df + t * t);
    const double tail = 0.5 * incomplete_beta(df / 2.0, 0.5, x);
    return t > 0 ? 1.0 - tail : tail;
}

double student_t_quantile(double p, double df) {
    if (p <= 0.0) return -std::numeric_limits<double>::infinity();
    if (p >= 1.0) return std::numeric_limits<double>::infinity();
    double lo = -1e4, hi = 1e4;
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (student_t_cdf(mid, df) < p) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

double chi2_sf(double x, double k) {
    if (x <= 0.0) return 1.0;
    return gamma_q(k / 2.0, x / 2.0);
}

double f_sf(double f, double d1, double d2) {
    if (!(f > 0.0)) return 1.0;
    const double x = d1 * f / (d1 * f + d2);
    return 1.0 - incomplete_beta(d1 / 2.0, d2 / 2.0, x);
}

WelchResult welch_t(const std::vector<double>& a, const std::vector<double>& b) {
    WelchResult r;
    if (a.size() < 2 || b.size() < 2) return r;
    const double ma = mean(a), mb = mean(b);
    const double va = variance(a) / double(a.size()), vb = variance(b) / double(b.size());
    r.diff = mb - ma;
    r.se = std::sqrt(va + vb);
    if (r.se <= 0.0) {
        r.df = double(a.size() + b.size() - 2);
        r.t = r.diff == 0.0 ? 0.0 : std::copysign(std::numeric_limits<double>::infinity(), r.diff);
        r.p_two_sided = r.diff == 0.0 ? 1.0 : 0.0;
        r.ci95 = {r.diff, r.diff};
        r.valid = true;
        return r;
    }
    const double num = (va + vb) * (va + vb);
    const double den = va * va / double(a.size() - 1) + vb * vb / double(b.size() - 1);
    r.df = den > 0 ? num / den : double(a.size() + b.size() - 2);
    r.t = r.diff / r.se;
    r.p_two_sided = 2.0 * (1.0 - student_t_cdf(std::fabs(r.t), r.df));
    const double q = student_t_quantile(0.975, r.df);
    r.ci95 = {r.diff - q * r.se, r.diff + q * r.se};
    r.valid = true;
    return r;
}

// ---- linear algebra ------------------------------------------------------------------------

Matrix Matrix::identity(size_t n) {
    Matrix m(n, n);
    for (size_t i = 0; i < n; ++i) m(i, i) = 1.0;
    return m;
}

Matrix transpose(const Matrix& m) {
    Matrix t(m.cols, m.rows);
    for (size_t r = 0; r < m.rows; ++r)
        for (size_t c = 0; c < m.cols; ++c) t(c, r) = m(r, c);
    return t;
}

Matrix multiply(const Matrix& x, const Matrix& y) {
    Matrix z(x.rows, y.cols);
    for (size_t i = 0; i < x.rows; ++i)
        for (size_t k = 0; k < x.cols; ++k) {
            const double v = x(i, k);
            if (v == 0.0) continue;
            for (size_t j = 0; j < y.cols; ++j) z(i, j) += v * y(k, j);
        }
    return z;
}

std::optional<Matrix> inverse(const Matrix& m) {
    if (m.rows != m.cols) return std::nullopt;
    const size_t n = m.rows;
    Matrix a = m, inv = Matrix::identity(n);
    double scale = 0.0;
    for (double v : m.a) scale = std::max(scale, std::fabs(v));
    const double tiny = std::max(scale, 1.0) * 1e-13;
    for (size_t col = 0; col < n; ++col) {
        size_t piv = col;
        for (size_t r = col + 1; r < n; ++r)
            if (std::fabs(a(r, col)) > std::fabs(a(piv, col))) piv = r;
        if (std::fabs(a(piv, col)) < tiny) return std::nullopt;
        if (piv != col)
            for (size_t c = 0; c < n; ++c) {
                std::swap(a(piv, c), a(col, c));
                std::swap(inv(piv, c), inv(col, c));
            }
        const double d = a(col, col);
        for (size_t c = 0; c < n; ++c) { a(col, c) /= d; inv(col, c) /= d; }
        for (size_t r = 0; r < n; ++r) {
            if (r == col) continue;
            const double f = a(r, col);
            if (f == 0.0) continue;
            for (size_t c = 0; c < n; ++c) { a(r, c) -= f * a(col, c); inv(r, c) -= f * inv(col, c); }
        }
    }
    return inv;
}

std::optional<std::vector<double>> solve(const Matrix& A, const std::vector<double>& b) {
    auto inv = inverse(A);
    if (!inv) return std::nullopt;
    std::vector<double> x(A.rows, 0.0);
    for (size_t i = 0; i < A.rows; ++i)
        for (size_t j = 0; j < A.cols; ++j) x[i] += (*inv)(i, j) * b[j];
    return x;
}

}  // namespace fit
