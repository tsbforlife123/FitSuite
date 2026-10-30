#include "fit/live_tuning.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "fit/optimize.hpp"

namespace fit {

const char* to_string(LiveFlick::Kind k) {
    switch (k) {
        case LiveFlick::Kind::Good: return "good";
        case LiveFlick::Kind::Overshoot: return "overshoot";
        case LiveFlick::Kind::Undershoot: return "undershoot";
        case LiveFlick::Kind::Sideways: return "sideways";
    }
    return "good";
}

// ---- finding flicks -------------------------------------------------------------------------------

namespace {
constexpr double kSpeedWindowUs = 5000.0;  // speed is measured over the last 5 ms of reports

Vec2 unit(Vec2 v) {
    const double l = length(v);
    return l > 0.0 ? Vec2{v.x / l, v.y / l} : Vec2{};
}
}  // namespace

void FlickDetector::reset() {
    st_ = State::Idle;
    pos_ = {};
    last_t_ = 0;
    hist_.clear();
    out_.clear();
}

void FlickDetector::begin_idle() {
    st_ = State::Idle;
    rest_pos_ = pos_;
    rest_t_ = last_t_;
}

double FlickDetector::speed_deg_s() const {
    if (hist_.size() < 2) return 0.0;
    const auto& a = hist_.front();
    const auto& b = hist_.back();
    const double dt_us = std::max(double(b.first - a.first), 125.0);
    return length(b.second - a.second) * o_.deg_per_count / (dt_us * 1e-6);
}

void FlickDetector::tick(int64_t now_us) {
    const double since_move_ms = double(now_us - last_move_t_) / 1000.0;
    switch (st_) {
        case State::Primary:
            // The mouse stopped dead without slowing down first: the first movement ended here.
            if (double(now_us - last_t_) / 1000.0 >= o_.settle_ms) {
                primary_end_ = pos_;
                st_ = State::Stopped;
                stop_pos_ = pos_;
                stop_t_ = last_t_;
            }
            break;
        case State::Correcting:
            if (since_move_ms >= o_.settle_ms) {
                st_ = State::Stopped;
                stop_pos_ = pos_;
                stop_t_ = last_move_t_;
            }
            break;
        case State::Stopped:
            if (double(now_us - stop_t_) / 1000.0 > o_.click_within_ms) begin_idle();  // stopped but no shot
            break;
        case State::Busy:
            if (since_move_ms >= o_.settle_ms) begin_idle();
            break;
        case State::Idle: break;
    }
}

void FlickDetector::add(const MoveSample& s) {
    if (last_t_ == 0) last_t_ = s.t_us;
    tick(s.t_us);  // time that passed without reports
    pos_ += Vec2{double(s.dx), double(s.dy)};
    hist_.push_back({s.t_us, pos_});
    while (hist_.size() > 2 && double(s.t_us - hist_.front().first) > kSpeedWindowUs) hist_.pop_front();
    // A long gap before this report: the speed is this report's movement over the gap.
    if (hist_.size() >= 2 && double(s.t_us - hist_[hist_.size() - 2].first) > kSpeedWindowUs)
        hist_.erase(hist_.begin(), hist_.end() - 2);
    const double v = speed_deg_s();
    const bool moving = v > o_.still_speed_deg_s && (s.dx != 0 || s.dy != 0);
    if (moving) last_move_t_ = s.t_us;
    last_t_ = s.t_us;

    switch (st_) {
        case State::Idle:
            if (v < o_.still_speed_deg_s) {
                rest_pos_ = pos_;
                rest_t_ = s.t_us;
            }
            if (v >= o_.start_speed_deg_s) {
                st_ = State::Primary;
                start_ = rest_pos_;
                start_t_ = rest_t_;
                peak_ = v;
                dir_ = unit(pos_ - start_);
            }
            break;
        case State::Primary: {
            if (s.click) {  // shot during the fast movement: nothing to judge it by
                st_ = State::Busy;
                break;
            }
            const Vec2 step{double(s.dx), double(s.dy)};
            const bool reversed = length(step) > 0.0 && dot(step, dir_) < 0.0;
            peak_ = std::max(peak_, v);
            if (length(pos_ - start_) > 0.0) dir_ = unit(pos_ - start_);
            if (double(s.t_us - start_t_) / 1000.0 > o_.max_primary_ms) {
                st_ = State::Busy;  // a long turn or tracking, not a flick
            } else if (v < 0.3 * peak_ || reversed) {
                primary_end_ = reversed ? pos_ - step : pos_;
                st_ = State::Correcting;
            }
            break;
        }
        case State::Correcting: {
            const double amp = length(primary_end_ - start_);
            if (double(s.t_us - start_t_) / 1000.0 > o_.max_flick_ms ||
                (v >= o_.start_speed_deg_s && length(pos_ - primary_end_) > 0.5 * amp)) {
                st_ = State::Busy;  // a new big movement, not a correction
            } else if (s.click) {
                if (v < 0.5 * o_.start_speed_deg_s) finish(s.t_us, pos_);  // shot as the aim arrived
                else st_ = State::Busy;
            }
            break;
        }
        case State::Stopped: {
            const double amp = length(stop_pos_ - start_);
            if (s.click) {
                finish(s.t_us, stop_pos_);
            } else if (moving && length(pos_ - stop_pos_) > std::max(2.0, 0.03 * amp)) {
                st_ = State::Busy;  // moved on without shooting
            }
            break;
        }
        case State::Busy: break;
    }
}

void FlickDetector::finish(int64_t t_us, Vec2 settled_pos) {
    st_ = State::Busy;  // wait for stillness before the next flick
    const Vec2 S = settled_pos - start_;
    const Vec2 P = primary_end_ - start_;
    const double amp = length(S);
    const double deg = amp * o_.deg_per_count;
    if (amp <= 0.0 || deg < o_.min_deg || deg > o_.max_deg) return;
    if (double(t_us - start_t_) / 1000.0 > o_.max_flick_ms) return;
    const Vec2 u = unit(S);
    const double along_ratio = dot(P, u) / amp;
    if (along_ratio < 0.5 || along_ratio > 1.6) return;  // not one aimed movement
    LiveFlick f;
    f.t_us = t_us;
    f.primary = P;
    f.settled = S;
    const Vec2 d = P - S;
    f.error_pct = length(d) / amp * 100.0;
    const double along = dot(d, u);
    const double side = length(d - Vec2{u.x * along, u.y * along});
    if (f.error_pct <= o_.good_pct) f.kind = LiveFlick::Kind::Good;
    else if (side > std::fabs(along)) f.kind = LiveFlick::Kind::Sideways;
    else f.kind = along > 0.0 ? LiveFlick::Kind::Overshoot : LiveFlick::Kind::Undershoot;
    out_.push_back(f);
}

std::vector<LiveFlick> FlickDetector::take() {
    std::vector<LiveFlick> r;
    r.swap(out_);
    return r;
}

// ---- nudging the fix ------------------------------------------------------------------------------

namespace {

struct Stat {
    double mean = 0.0, se = 0.0;
    int n = 0;
};

Stat stat_of(std::vector<double> v) {
    Stat s;
    s.n = int(v.size());
    if (v.empty()) return s;
    // Trimmed: the outer 10% at each end are left out, so a misread flick cannot pull the mean.
    std::sort(v.begin(), v.end());
    const size_t cut = v.size() >= 10 ? v.size() / 10 : 0;
    std::vector<double> t(v.begin() + long(cut), v.end() - long(cut));
    double m = 0.0;
    for (double x : t) m += x;
    m /= double(t.size());
    double ss = 0.0;
    for (double x : t) ss += (x - m) * (x - m);
    s.mean = m;
    s.se = t.size() > 1 ? std::sqrt(ss / double(t.size() - 1)) / std::sqrt(double(t.size())) : 1e9;
    return s;
}

double mean_log_gain(const RawAccelProfile& p) {
    double s = 0.0;
    for (int i = 0; i < 72; ++i) s += std::log(p.gain(double(i) * kTwoPi / 72.0));
    return s / 72.0;
}

// Directions are bounded relative to the overall speed (which has its own limit).
bool within(const RawAccelProfile& p, const Bounds& b) {
    if (std::fabs(p.rotation_deg) > b.max_angle_deg + 1e-9) return false;
    const double overall = std::exp(mean_log_gain(p));
    for (int i = 0; i < 72; ++i)
        if (std::fabs(p.gain(double(i) * kTwoPi / 72.0) / overall - 1.0) > b.max_gain_deviation + 1e-9) return false;
    return true;
}

}  // namespace

std::optional<RawAccelProfile> LiveTuner::propose(const RawAccelProfile& current) {
    if (int(flicks_.size()) < o_.min_flicks) return std::nullopt;
    std::array<std::vector<double>, 8> by_dir;
    std::vector<double> angles;
    for (const auto& f : flicks_) {
        const double amp = length(f.settled);
        if (amp <= 0.0) continue;
        const Vec2 u{f.settled.x / amp, f.settled.y / amp};
        const double along = std::max(dot(f.primary, u), 0.3 * amp);
        const double theta = screen_angle(f.settled);
        const int b = int(std::floor((theta + kPi / 8.0 + kTwoPi) / (kPi / 4.0))) % 8;
        by_dir[size_t(b)].push_back(std::log(amp / along));  // > 0: fell short, needs more gain here
        angles.push_back(signed_screen_angle_between(f.primary, f.settled));
    }
    flicks_.clear();

    std::array<Stat, 8> st;
    int used = 0;
    double centre = 0.0;
    for (size_t b = 0; b < 8; ++b) {
        st[b] = stat_of(by_dir[b]);
        if (st[b].n >= o_.min_per_direction) {
            ++used;
            centre += st[b].mean;
        }
    }
    if (used < 3) return std::nullopt;
    centre /= double(used);  // the directions are judged against this average
    // Overall speed: short (or long) in every direction alike. Its own, slower step and limit.
    double doverall = 0.0;
    if (o_.overall) {
        double var = 0.0;
        for (size_t b = 0; b < 8; ++b)
            if (st[b].n >= o_.min_per_direction) var += st[b].se * st[b].se;
        const double se = std::sqrt(var) / double(used);
        if (std::fabs(centre) >= o_.min_t * se)
            doverall = std::clamp(o_.rate * centre, -o_.max_overall_step_pct / 100.0, o_.max_overall_step_pct / 100.0);
    }

    std::array<double, 8> target{};
    bool any = false;
    for (size_t b = 0; b < 8; ++b) {
        const double theta = double(b) * kPi / 4.0;
        target[b] = std::log(current.gain(theta));
        if (st[b].n < o_.min_per_direction) continue;
        const double diff = st[b].mean - centre;
        if (std::fabs(diff) < o_.min_t * st[b].se) continue;
        const double step = std::clamp(o_.rate * diff, -o_.max_step_pct / 100.0, o_.max_step_pct / 100.0);
        target[b] += step;
        any = any || std::fabs(step) > 1e-6;
    }
    const Stat rot = stat_of(angles);
    double drot = 0.0;
    if (rot.n >= o_.min_flicks && std::fabs(rot.mean) >= o_.min_t * rot.se)
        drot = std::clamp(o_.rate * rot.mean * kRadToDeg, -o_.max_step_deg, o_.max_step_deg);
    const double lim = std::log(1.0 + o_.max_overall);
    const double overall_now = mean_log_gain(current);
    const double overall_next = std::clamp(overall_now + doverall, std::min(-lim, overall_now), std::max(lim, overall_now));
    if (!any && std::fabs(drot) < 1e-6 && std::fabs(overall_next - overall_now) < 1e-6) return std::nullopt;

    RawAccelProfile base = current;
    base.rotation_deg = current.rotation_deg + drot;
    auto make = [&](const std::vector<double>& x) {
        RawAccelProfile p = base;
        p.sens = current.sens * std::exp(x[0]);
        p.yx_ratio = current.yx_ratio * std::exp(x[1]);
        p.lr_ratio = current.lr_ratio * std::exp(x[2]);
        p.ud_ratio = current.ud_ratio * std::exp(x[3]);
        return p;
    };
    auto cost = [&](const std::vector<double>& x) {
        const RawAccelProfile p = make(x);
        double c = 0.0;
        for (size_t b = 0; b < 8; ++b) {
            const double e = std::log(p.gain(double(b) * kPi / 4.0)) - target[b];
            c += e * e;
        }
        return c;
    };
    const auto nm = nelder_mead(cost, std::vector<double>(4, 0.0), 0.01, 3000);
    RawAccelProfile next = make(nm.x);
    auto set_overall = [&](RawAccelProfile& p) { p.sens *= std::exp(overall_next - mean_log_gain(p)); };
    set_overall(next);

    // Never past the bounds of the measured fix: the direction part is scaled back toward none until
    // it fits; the overall speed stays as set.
    if (!within(next, o_.bounds)) {
        RawAccelProfile scaled = next;
        double lo = 0.0, hi = 1.0;
        for (int i = 0; i < 30; ++i) {
            const double s = 0.5 * (lo + hi);
            RawAccelProfile p = next;
            p.rotation_deg = next.rotation_deg * s;
            p.yx_ratio = std::exp(std::log(next.yx_ratio) * s);
            p.lr_ratio = std::exp(std::log(next.lr_ratio) * s);
            p.ud_ratio = std::exp(std::log(next.ud_ratio) * s);
            set_overall(p);
            if (within(p, o_.bounds)) {
                lo = s;
                scaled = p;
            } else {
                hi = s;
            }
        }
        next = scaled;
    }
    return next;
}

}  // namespace fit
