// Deterministic random numbers. std::*_distribution differs between standard libraries,
// so target sequences and bootstrap draws use these hand-written distributions instead:
// the same seed gives the same sequence on Windows (libc++/MSVC) and in Linux tests.
#pragma once

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace fit {

class Rng {
public:
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) { reseed(seed); }

    void reseed(uint64_t seed) {
        uint64_t z = seed;
        for (auto& s : s_) s = splitmix(z);
        has_spare_ = false;
    }

    uint64_t next_u64() {
        const uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    // Uniform in [0, 1).
    double uniform() { return double(next_u64() >> 11) * (1.0 / 9007199254740992.0); }
    double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }

    // Uniform integer in [0, n). Unbiased (rejection sampling).
    uint64_t below(uint64_t n) {
        if (n <= 1) return 0;
        const uint64_t limit = UINT64_MAX - (UINT64_MAX % n);
        uint64_t v;
        do { v = next_u64(); } while (v >= limit);
        return v % n;
    }

    // Standard normal (Marsaglia polar method).
    double normal() {
        if (has_spare_) { has_spare_ = false; return spare_; }
        double u, v, s;
        do {
            u = uniform() * 2.0 - 1.0;
            v = uniform() * 2.0 - 1.0;
            s = u * u + v * v;
        } while (s >= 1.0 || s == 0.0);
        const double m = std::sqrt(-2.0 * std::log(s) / s);
        spare_ = v * m;
        has_spare_ = true;
        return u * m;
    }
    double normal(double mean, double sd) { return mean + sd * normal(); }

    template <class T>
    void shuffle(std::vector<T>& v) {
        for (size_t i = v.size(); i > 1; --i) {
            const size_t j = size_t(below(i));
            std::swap(v[i - 1], v[j]);
        }
    }

private:
    static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    static uint64_t splitmix(uint64_t& z) {
        z += 0x9E3779B97F4A7C15ull;
        uint64_t r = z;
        r = (r ^ (r >> 30)) * 0xBF58476D1CE4E5B9ull;
        r = (r ^ (r >> 27)) * 0x94D049BB133111EBull;
        return r ^ (r >> 31);
    }

    uint64_t s_[4]{};
    bool has_spare_ = false;
    double spare_ = 0.0;
};

}  // namespace fit
