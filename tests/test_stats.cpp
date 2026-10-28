#include <cmath>

#include "doctest.h"
#include "fit/rng.hpp"
#include "fit/stats.hpp"

using namespace fit;

TEST_CASE("percentile uses linear interpolation (type 7)") {
    std::vector<double> v{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK(percentile(v, 0) == doctest::Approx(1.0));
    CHECK(percentile(v, 50) == doctest::Approx(5.5));
    CHECK(percentile(v, 90) == doctest::Approx(9.1));
    CHECK(percentile(v, 99) == doctest::Approx(9.91));
    CHECK(percentile(v, 100) == doctest::Approx(10.0));
    CHECK(median({3, 1, 2}) == doctest::Approx(2.0));
}

TEST_CASE("distribution functions match reference values") {
    CHECK(normal_cdf(1.959963985) == doctest::Approx(0.975).epsilon(1e-6));
    CHECK(normal_quantile(0.975) == doctest::Approx(1.959963985).epsilon(1e-6));
    CHECK(normal_quantile(0.001) == doctest::Approx(-3.090232306).epsilon(1e-6));
    CHECK(student_t_quantile(0.975, 10) == doctest::Approx(2.228138852).epsilon(1e-5));
    CHECK(student_t_quantile(0.975, 3) == doctest::Approx(3.182446305).epsilon(1e-5));
    CHECK(student_t_cdf(0.0, 7) == doctest::Approx(0.5));
    CHECK(chi2_sf(3.841458821, 1) == doctest::Approx(0.05).epsilon(1e-5));
    CHECK(chi2_sf(9.487729037, 4) == doctest::Approx(0.05).epsilon(1e-5));
    CHECK(f_sf(4.102821015, 2, 10) == doctest::Approx(0.05).epsilon(1e-5));
}

TEST_CASE("Welch t-test") {
    std::vector<double> a{10, 11, 9, 10.5, 9.5}, b{12, 13, 11, 12.5, 11.5};
    auto w = welch_t(a, b);
    REQUIRE(w.valid);
    CHECK(w.diff == doctest::Approx(2.0));
    CHECK(w.ci95.lo > 0.0);
    CHECK(w.p_two_sided < 0.01);
    auto same = welch_t(a, a);
    CHECK(same.p_two_sided == doctest::Approx(1.0));
    CHECK_FALSE(welch_t({1.0}, b).valid);
}

TEST_CASE("matrix inverse and solve") {
    Matrix m(2, 2);
    m(0, 0) = 4; m(0, 1) = 7; m(1, 0) = 2; m(1, 1) = 6;
    auto inv = inverse(m);
    REQUIRE(inv);
    CHECK((*inv)(0, 0) == doctest::Approx(0.6));
    CHECK((*inv)(0, 1) == doctest::Approx(-0.7));
    Matrix s(2, 2);
    s(0, 0) = 1; s(0, 1) = 2; s(1, 0) = 2; s(1, 1) = 4;
    CHECK_FALSE(inverse(s).has_value());
}

TEST_CASE("RNG is deterministic and distributions are sane") {
    Rng a(42), b(42);
    for (int i = 0; i < 100; ++i) CHECK(a.next_u64() == b.next_u64());
    Rng r(1);
    std::vector<double> n;
    for (int i = 0; i < 20000; ++i) n.push_back(r.normal());
    CHECK(mean(n) == doctest::Approx(0.0).epsilon(0.03));
    CHECK(stddev(n) == doctest::Approx(1.0).epsilon(0.03));
    for (int i = 0; i < 1000; ++i) {
        const auto v = r.below(7);
        CHECK(v < 7);
    }
}
