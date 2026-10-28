// Descriptive statistics, distributions and small linear algebra used across the core.
#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace fit {

// ---- descriptive ---------------------------------------------------------------------------
double mean(const std::vector<double>& v);
// Sample variance (n-1 denominator). Returns 0 for n < 2.
double variance(const std::vector<double>& v);
double stddev(const std::vector<double>& v);
double median(std::vector<double> v);
// Percentile by linear interpolation between order statistics (Hyndman & Fan type 7, the
// default of R/NumPy/Excel PERCENTILE.INC). p in [0, 100].
double percentile(std::vector<double> v, double p);
double percentile_sorted(const std::vector<double>& sorted, double p);
// Median absolute deviation scaled by 1.4826 (consistent with SD for normal data).
double mad(const std::vector<double>& v);

struct Interval {
    double lo = 0.0;
    double hi = 0.0;
    bool contains(double x) const { return x >= lo && x <= hi; }
};

// Percentile interval of a bootstrap distribution (e.g. 95% => 2.5th..97.5th).
Interval percentile_interval(std::vector<double> draws, double level = 0.95);

// ---- distributions -------------------------------------------------------------------------
double normal_cdf(double z);
double normal_quantile(double p);
// Regularized incomplete beta I_x(a, b).
double incomplete_beta(double a, double b, double x);
// Regularized lower/upper incomplete gamma P(a, x), Q(a, x).
double gamma_p(double a, double x);
double gamma_q(double a, double x);
double student_t_cdf(double t, double df);
double student_t_quantile(double p, double df);
// Survival function of the chi-square distribution: P(X >= x), k degrees of freedom.
double chi2_sf(double x, double k);
// Survival function of the F distribution with (d1, d2) degrees of freedom.
double f_sf(double f, double d1, double d2);

struct WelchResult {
    double diff = 0.0;      // mean_b - mean_a
    double se = 0.0;
    double df = 0.0;
    double t = 0.0;
    double p_two_sided = 1.0;
    Interval ci95;          // for diff
    bool valid = false;     // false when either group has < 2 observations
};
WelchResult welch_t(const std::vector<double>& a, const std::vector<double>& b);

// ---- small dense linear algebra (row-major) -----------------------------------------------
struct Matrix {
    size_t rows = 0, cols = 0;
    std::vector<double> a;

    Matrix() = default;
    Matrix(size_t r, size_t c, double fill = 0.0) : rows(r), cols(c), a(r * c, fill) {}
    double& operator()(size_t r, size_t c) { return a[r * cols + c]; }
    double operator()(size_t r, size_t c) const { return a[r * cols + c]; }
    static Matrix identity(size_t n);
};

Matrix transpose(const Matrix& m);
Matrix multiply(const Matrix& x, const Matrix& y);
// Inverse via Gauss-Jordan with partial pivoting. nullopt when singular.
std::optional<Matrix> inverse(const Matrix& m);
// Solve A x = b. nullopt when singular.
std::optional<std::vector<double>> solve(const Matrix& A, const std::vector<double>& b);

}  // namespace fit
