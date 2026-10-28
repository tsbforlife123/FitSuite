#include "fit/timeutil.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>

#include "fit/rng.hpp"

namespace fit {

namespace {

// Howard Hinnant's civil-from-days / days-from-civil.
void civil_from_days(int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = int64_t(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = int(yy + (m <= 2));
}

int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}

}  // namespace

int64_t unix_now() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string utc_iso_from_unix(int64_t t) {
    int64_t days = t / 86400;
    int64_t rem = t % 86400;
    if (rem < 0) { rem += 86400; --days; }
    int y;
    unsigned m, d;
    civil_from_days(days, y, m, d);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02dZ", y, m, d, int(rem / 3600), int(rem % 3600 / 60),
                  int(rem % 60));
    return buf;
}

std::string utc_now_iso() { return utc_iso_from_unix(unix_now()); }

bool parse_utc_iso(const std::string& s, int64_t& out) {
    int y, mo, d, h, mi, se;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ", &y, &mo, &d, &h, &mi, &se) != 6) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 60) return false;
    out = days_from_civil(y, unsigned(mo), unsigned(d)) * 86400 + h * 3600 + mi * 60 + se;
    return true;
}

std::string utc_compact_now() {
    const std::string iso = utc_now_iso();  // YYYY-MM-DDTHH:MM:SSZ
    return iso.substr(0, 4) + iso.substr(5, 2) + iso.substr(8, 2) + "-" + iso.substr(11, 2) + iso.substr(14, 2) +
           iso.substr(17, 2);
}

std::string make_id(const std::string& prefix) {
    static std::atomic<uint64_t> counter{0};
    static const uint64_t boot = [] {
        uint64_t s = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
        try {
            std::random_device rd;
            s ^= (uint64_t(rd()) << 32) ^ uint64_t(rd());
        } catch (...) {
        }
        return s;
    }();
    Rng r(boot ^ (counter.fetch_add(1) * 0x9E3779B97F4A7C15ull) ^
          uint64_t(std::chrono::system_clock::now().time_since_epoch().count()));
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)r.next_u64());
    return prefix + "-" + buf;
}

}  // namespace fit
