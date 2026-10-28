// Minimal Nelder–Mead simplex minimiser for small, smooth problems (≤ 6 parameters).
#pragma once

#include <algorithm>
#include <functional>
#include <vector>

namespace fit {

struct NelderMeadResult {
    std::vector<double> x;
    double value = 0.0;
    int iterations = 0;
};

inline NelderMeadResult nelder_mead(const std::function<double(const std::vector<double>&)>& f,
                                    std::vector<double> x0, double step = 0.1, int max_iter = 2000,
                                    double tol = 1e-12) {
    const size_t n = x0.size();
    std::vector<std::vector<double>> s(n + 1, x0);
    for (size_t i = 0; i < n; ++i) s[i + 1][i] += step;
    std::vector<double> fv(n + 1);
    for (size_t i = 0; i <= n; ++i) fv[i] = f(s[i]);
    std::vector<size_t> order(n + 1);
    int it = 0;
    for (; it < max_iter; ++it) {
        for (size_t i = 0; i <= n; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return fv[a] < fv[b]; });
        const size_t best = order[0], worst = order[n], second = order[n - 1];
        if (std::abs(fv[worst] - fv[best]) <= tol * (std::abs(fv[best]) + tol)) break;
        std::vector<double> c(n, 0.0);
        for (size_t i = 0; i <= n; ++i)
            if (i != worst)
                for (size_t j = 0; j < n; ++j) c[j] += s[i][j] / double(n);
        auto lerp = [&](double t) {
            std::vector<double> p(n);
            for (size_t j = 0; j < n; ++j) p[j] = c[j] + t * (s[worst][j] - c[j]);
            return p;
        };
        auto xr = lerp(-1.0);
        const double fr = f(xr);
        if (fr < fv[best]) {
            auto xe = lerp(-2.0);
            const double fe = f(xe);
            if (fe < fr) { s[worst] = xe; fv[worst] = fe; } else { s[worst] = xr; fv[worst] = fr; }
        } else if (fr < fv[second]) {
            s[worst] = xr; fv[worst] = fr;
        } else {
            const bool outside = fr < fv[worst];
            auto xc = lerp(outside ? -0.5 : 0.5);
            const double fc = f(xc);
            if (fc < (outside ? fr : fv[worst])) {
                s[worst] = xc; fv[worst] = fc;
            } else {
                for (size_t i = 0; i <= n; ++i) {
                    if (i == best) continue;
                    for (size_t j = 0; j < n; ++j) s[i][j] = s[best][j] + 0.5 * (s[i][j] - s[best][j]);
                    fv[i] = f(s[i]);
                }
            }
        }
    }
    size_t b = 0;
    for (size_t i = 1; i <= n; ++i)
        if (fv[i] < fv[b]) b = i;
    return {s[b], fv[b], it};
}

}  // namespace fit
