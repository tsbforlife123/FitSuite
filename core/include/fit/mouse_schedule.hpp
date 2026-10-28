// MouseFit target plans and trial ordering.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fit/mouse_types.hpp"
#include "fit/rng.hpp"

namespace fit {

struct CalibrationPlanOptions {
    std::vector<double> distances_deg{6.0, 14.0, 26.0};
    int directions = 8;           // evenly spaced, starting at 0° (right)
    int blocks = 4;               // each block contains every direction × distance once
    double radius_deg = 1.2;
    uint64_t seed = 1;
};

// Round-based calibration (format 2): short rounds like an aim trainer. Each round starts with a
// click; a new target appears the moment the last one was hit or missed, and every target has its
// own timer. Flick and micro rounds alternate a target out from the round's centre and one back to
// it, which keeps the view near the horizon and gives every direction its opposite.
struct RoundPlanOptions {
    std::vector<double> flick_distances_deg{7.0, 13.0, 20.0, 26.0};
    std::vector<double> micro_distances_deg{2.0, 4.0};
    int directions = 8;
    int flick_rounds = 2;          // each: directions × distances targets out + the same number back
    bool micro_round = true;
    bool switch_round = true;
    bool track_round = true;
    double flick_radius_deg = 1.2, micro_radius_deg = 0.6, switch_radius_deg = 1.0, track_radius_deg = 1.3;
    double flick_timeout_ms = 2200.0, micro_timeout_ms = 2200.0, switch_timeout_ms = 3000.0;
    int switch_kills = 36, switch_visible = 3;
    double track_duration_ms = 40000.0;
    uint64_t seed = 1;
};

struct ValidationPlanOptions {
    // Validation uses directions and distances that calibration never used (fresh sequences).
    std::vector<double> distances_deg{9.0, 20.0};
    int directions = 8;
    double direction_offset_deg = 22.5;
    double direction_jitter_deg = 6.0;
    double distance_jitter_frac = 0.12;
    int blocks = 8;               // multiple of 4: counterbalanced ABBA BAAB order
    int warmup_trials = 8;        // baseline, not scored
    double radius_deg = 1.2;
    double target_timeout_ms = 2200.0;
    uint64_t seed = 2;
};

// How one block is played in a round-based plan.
struct BlockInfo {
    Task task = Task::Flick;
    double target_timeout_ms = 2200.0;  // each target's timer
    int visible_targets = 1;            // Switch: targets on screen at once
    int kills = 0;                      // Switch: targets to shoot
    double duration_ms = 0.0;           // Track: length of the round
    double radius_deg = 1.2;            // Switch / Track target size
};

struct Plan {
    SessionKind kind = SessionKind::Calibration;
    uint64_t seed = 0;
    std::vector<TargetSpec> warmup;
    std::vector<std::vector<TargetSpec>> blocks;
    std::vector<Condition> block_condition;  // per block
    std::vector<BlockInfo> block_info;       // per block; empty = format 1 (hold still between targets)
    BlockInfo warmup_info;
    std::string order_label;                 // e.g. "ABBABAAB" (A = baseline); never shown during the test
    int format() const { return block_info.empty() ? 1 : 2; }
    size_t scored_trials() const;            // targets to shoot (Track rounds count none)
};

// Format 1 (FitSuite 0.1): one target at a time after holding still. Kept for the statistics tests
// and for reading old sessions.
Plan make_calibration_plan(const CalibrationPlanOptions& o);
// Format 2: flick, micro, flick, tracking and switching rounds (about 4 minutes).
Plan make_round_calibration_plan(const RoundPlanOptions& o);
// Format 2: blocks of out-and-back targets in the counterbalanced order.
Plan make_validation_plan(const ValidationPlanOptions& o);
// Out-and-back pairs: for each outward spec (dir/dist from the crosshair), a target back at the centre.
std::vector<TargetSpec> out_and_back(const std::vector<TargetSpec>& outward);

// Picks targets from a block so the camera stays near the horizon (where count-space direction
// matches what the player sees), inserting unscored positioning targets when needed.
class TrialPicker {
public:
    TrialPicker(std::vector<TargetSpec> specs, uint64_t seed, double pitch_soft_deg = 8.0, double pitch_hard_deg = 16.0);

    bool done() const { return remaining_.empty(); }
    size_t remaining() const { return remaining_.size(); }
    size_t total() const { return total_; }

    // Next target for the current camera pitch. Does not remove it; call complete() when the trial
    // ends with a hit or timeout. Interrupted trials stay queued so blocks remain balanced.
    TargetSpec next(double current_pitch_deg);
    void complete(int spec_index);

private:
    std::vector<TargetSpec> remaining_;
    size_t total_;
    Rng rng_;
    double soft_, hard_;
    int positioning_counter_ = 0;
    int consecutive_positioning_ = 0;
};

}  // namespace fit
