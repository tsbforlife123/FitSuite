// Per-round results of a round-based calibration (format 2): what an aim trainer would show after
// each round, plus how well the player tracks targets moving in each direction.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "fit/mouse_types.hpp"

namespace fit {

struct TrackingResult {
    bool ok = false;
    int samples = 0;
    double on_target_pct = 0.0;   // share of time the crosshair was on the target or its trail (kTrackTrailMs)
    double mean_error_deg = 0.0;  // average distance from the target centre
    // How far behind the target the crosshair stays while it moves steadily, as time (distance
    // behind along the target's path ÷ its speed), by the direction the target was moving (0 = right,
    // 1 = up-right, ... counter-clockwise). Everyone trails by a reaction time; a direction where
    // the player trails clearly more is a directional weakness. NaN where there was too little data.
    std::array<double, 8> lag_ms{};
    std::array<int, 8> lag_n{};
    std::string summary;          // plain language
};

// Tracking samples of one round (round < 0: all tracking samples).
TrackingResult analyze_tracking(const std::vector<TrackSample>& samples, int round = -1);

struct RoundReport {
    int round = 0;
    Task task = Task::Flick;
    Condition condition = Condition::Baseline;
    int targets = 0;              // targets shown (hits + misses + timeouts)
    int hits = 0, misses = 0, timeouts = 0;
    double accuracy_pct = 0.0;    // hits / targets
    double median_time_ms = 0.0;  // target appearing (or the last kill, when switching) to the hit
    double kills_per_min = 0.0;
    double duration_s = 0.0;
    TrackingResult tracking;      // Track rounds only
};

struct AimReport {
    std::vector<RoundReport> rounds;
    std::vector<std::string> lines;  // one plain-language line per kind of round
};

AimReport make_aim_report(const Session& s);

const char* task_title(Task t);  // "Flicks", "Micro-adjustments", "Target switching", "Tracking"

}  // namespace fit
