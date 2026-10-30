// Live tuning: after the calibration, FitSuite keeps learning from the player's own flicks in their
// games and nudges the fix while they play.
//
// It sees only mouse movement and clicks (never the screen or the game), so it judges a flick by how
// the player finished it: a fast first movement (the flick), small corrections, then a stop and a
// click. Where they stopped and clicked is taken as where they meant to go. A flick whose first
// movement already ended there is good; one that needed a correction overshot, undershot or went
// sideways. When flicks in some directions keep falling short or long compared with the others, the
// fix is nudged a little in those directions: small steps, at most every few minutes, always within
// the same bounds as the measured fix (8% per direction, 3° of rotation). When every direction lands
// short (or long), the overall speed moves too, more slowly (0.5% per change, 8% in total).
#pragma once

#include <cstdint>
#include <deque>
#include <utility>
#include <optional>
#include <vector>

#include "fit/common.hpp"
#include "fit/mouse_transform.hpp"

namespace fit {

struct MoveSample {
    int64_t t_us = 0;
    int32_t dx = 0, dy = 0;  // counts as the game receives them (after Raw Accel)
    bool click = false;      // left button pressed
};

struct LiveFlick {
    enum class Kind { Good, Overshoot, Undershoot, Sideways };
    int64_t t_us = 0;   // when it was clicked
    Vec2 primary;       // where the fast first movement ended, from the start (counts)
    Vec2 settled;       // where the player stopped and clicked, from the start (counts)
    Kind kind = Kind::Good;
    double error_pct = 0.0;  // |primary − settled| / |settled| × 100
};
const char* to_string(LiveFlick::Kind k);

struct FlickDetectorOptions {
    double deg_per_count = 0.028;     // the game's turn per count (turn rate × sensitivity)
    double start_speed_deg_s = 150.0; // a movement this fast starts a flick
    double still_speed_deg_s = 20.0;  // slower than this counts as still
    double settle_ms = 90.0;          // still this long: stopped
    double click_within_ms = 350.0;   // a click this soon after stopping makes it an aimed flick
    double max_flick_ms = 1000.0;     // start to stop
    double max_primary_ms = 450.0;    // longer fast movements are turns or tracking, not flicks
    double min_deg = 3.0, max_deg = 60.0;
    double good_pct = 8.0;            // first movement within this of the stop: a good flick
};

// Finds aimed flicks in a stream of mouse reports.
class FlickDetector {
public:
    explicit FlickDetector(FlickDetectorOptions o = {}) : o_(o) {}
    void add(const MoveSample& s);
    // Lets time pass without reports (the mouse is still); call it regularly.
    void tick(int64_t now_us);
    std::vector<LiveFlick> take();
    void reset();

private:
    enum class State { Idle, Primary, Correcting, Stopped, Busy };
    void begin_idle();
    void finish(int64_t t_us, Vec2 settled);
    double speed_deg_s() const;  // over the last few milliseconds of reports

    FlickDetectorOptions o_;
    State st_ = State::Idle;
    Vec2 pos_{};              // running position (counts)
    int64_t last_t_ = 0;
    Vec2 rest_pos_{};         // where the mouse last stood still (a flick starts from here)
    int64_t rest_t_ = 0;
    Vec2 start_{}, primary_end_{}, stop_pos_{};
    int64_t start_t_ = 0, last_move_t_ = 0, stop_t_ = 0;
    double peak_ = 0.0;
    Vec2 dir_{};              // direction of the first movement
    std::deque<std::pair<int64_t, Vec2>> hist_;  // recent reports: time, position
    std::vector<LiveFlick> out_;
};

struct LiveTunerOptions {
    int min_flicks = 40;          // aimed flicks needed before a change
    int min_per_direction = 4;    // in at least 3 of the 8 directions
    double rate = 0.5;            // share of the seen difference applied per change
    double max_step_pct = 1.0;    // largest change per direction per update
    double max_step_deg = 0.3;    // largest rotation change per update
    double min_t = 3.0;           // a direction counts only when its difference is this clear (mean / standard error)
    bool overall = true;          // also change the overall speed when every direction lands short (or long)
    double max_overall_step_pct = 0.5;  // per update
    double max_overall = 0.08;    // in total, either way
    Bounds bounds{};
};

// Collects aimed flicks and proposes a nudged fix when they show a clear pattern by direction.
class LiveTuner {
public:
    explicit LiveTuner(LiveTunerOptions o = {}) : o_(o) {}
    void add(const LiveFlick& f) { flicks_.push_back(f); }
    size_t pending() const { return flicks_.size(); }
    // The current fix nudged toward what the flicks show, or nothing when they show no clear pattern
    // (or too few flicks). The flicks used are cleared either way once there were enough.
    std::optional<RawAccelProfile> propose(const RawAccelProfile& current);

private:
    LiveTunerOptions o_;
    std::vector<LiveFlick> flicks_;
};

}  // namespace fit
