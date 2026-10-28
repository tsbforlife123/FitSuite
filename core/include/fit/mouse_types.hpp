// MouseFit trial data model.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fit/common.hpp"

namespace fit {

// How a game turns mouse counts into view rotation. VALORANT: 0.07° per count at sensitivity 1,
// identical for yaw and pitch, no acceleration.
struct GameMapping {
    std::string game_id = "valorant";
    double sensitivity = 0.4;
    double deg_per_count_at_sens1 = 0.07;
    double hfov_deg_16x9 = 103.0;  // VALORANT's fixed horizontal FOV at 16:9

    double deg_per_count() const { return deg_per_count_at_sens1 * sensitivity; }
    bool valid() const { return sensitivity > 0.0 && sensitivity < 100.0 && deg_per_count_at_sens1 > 0.0; }
};

enum class Condition : uint8_t { Baseline = 0, Candidate = 1 };
enum class TrialPurpose : uint8_t { Scored = 0, Recenter = 1, Warmup = 2 };
// Miss: a click off the target. In round-based sessions the target then disappears (like Aim Lab).
enum class TrialOutcome : uint8_t { Hit = 0, Timeout = 1, Interrupted = 2, Miss = 3 };
// Kind of round. Flick: one target at a time, alternating out from the round's centre and back.
// Micro: the same with small, close targets (micro-adjustments). Switch: several targets visible at
// once, shoot them in any order. Track: keep the crosshair on a moving target (no clicking).
enum class Task : uint8_t { Flick = 0, Micro = 1, Switch = 2, Track = 3 };
// Check: a short session with the chosen fix in use, to see whether it still fits (mouse_check.hpp).
enum class SessionKind : uint8_t { Calibration = 0, Validation = 1, Check = 2 };

const char* to_string(Condition c);
const char* to_string(TrialPurpose p);
const char* to_string(TrialOutcome o);
const char* to_string(SessionKind k);
const char* to_string(Task t);

struct TargetSpec {
    double dir_deg = 0.0;     // screen convention: 0 = right, 90 = up
    double dist_deg = 10.0;   // required rotation, game degrees
    double radius_deg = 1.2;  // target angular radius
    int block = 0;
    int index = 0;            // position in the plan (stable id)
    Condition condition = Condition::Baseline;
    TrialPurpose purpose = TrialPurpose::Scored;
    Task task = Task::Flick;
    // Placed at the round's centre point instead of dir/dist from the crosshair (the "back" target of
    // an out-and-back pair). The test fills in the actual dir/dist when the target appears.
    bool to_anchor = false;
    // Distance level the target belongs to (the planned distance); 0 = dist_deg. Analyses group by it.
    double nominal_dist_deg = 0.0;
    double level_deg() const { return nominal_dist_deg > 0.0 ? nominal_dist_deg : dist_deg; }
};

// One raw input report and what the active transform turned it into.
struct MotionSample {
    int64_t t_us = 0;   // monotonic timestamp of the report
    int32_t in_dx = 0;  // raw counts
    int32_t in_dy = 0;
    double out_dx = 0;  // counts after the active transform (what moved the crosshair)
    double out_dy = 0;
};

struct ClickEvent {
    int64_t t_us = 0;
    bool hit = false;
    double offset_deg = 0.0;  // angular distance from target centre at the click
};

struct Trial {
    TargetSpec spec;
    // Output-space counts needed to land on the target centre (raw coordinates). Exact for a
    // yaw/pitch camera: (Δyaw, -Δpitch) / deg_per_count.
    Vec2 target_counts;
    double start_yaw_deg = 0.0;
    double start_pitch_deg = 0.0;
    int64_t t_appear_us = 0;
    int64_t t_end_us = 0;  // time of hit, miss, timeout or interruption
    double timeout_ms = 3000.0;  // how long the target stayed up at most (its timer)
    TrialOutcome outcome = TrialOutcome::Interrupted;
    std::vector<MotionSample> motion;  // reports from target onset to t_end
    std::vector<ClickEvent> clicks;
};

// Conditions under which a session was recorded. Stored with every session so results can be
// interpreted (and flagged as stale) later.
struct SessionConditions {
    std::string mouse_name;
    std::string mouse_id;        // stable hash of the device path, or empty
    std::string mouse_vid_pid;   // "046D:C08B" when known
    int dpi = 800;
    double polling_hz_measured = 0.0;
    GameMapping mapping;
    double display_refresh_hz = 0.0;
    int display_width = 0, display_height = 0;
    double render_fps_median = 0.0;
    double render_frame_p99_ms = 0.0;
    int render_hitches = 0;        // frames > 3× median
    bool tearing_allowed = false;
    std::string data_origin = "measured";  // "measured" or "simulated" (dev harness / tests only)
};

// One round of a round-based session (format 2).
struct RoundRecord {
    int index = 0;
    Task task = Task::Flick;
    Condition condition = Condition::Baseline;
    int64_t t_start_us = 0, t_end_us = 0;
    int hits = 0, misses = 0, timeouts = 0;
    bool complete = false;
};

// Tracking rounds: one sample per rendered frame. Angles in degrees, velocities in degrees/second.
struct TrackSample {
    int64_t t_us = 0;
    int round = 0;
    float target_yaw = 0, target_pitch = 0;
    float aim_yaw = 0, aim_pitch = 0;
    float target_vyaw = 0, target_vpitch = 0;
    float radius = 1.0f;
};

struct Session {
    std::string id;
    // 1: one target at a time after holding still (FitSuite 0.1). 2: rounds with targets appearing
    // right after the last one was hit or missed, each with its own timer.
    int format = 1;
    SessionKind kind = SessionKind::Calibration;
    std::string started_utc;
    std::string finished_utc;
    bool complete = false;
    std::string algorithm_version;
    uint64_t seed = 0;
    SessionConditions conditions;
    // Validation sessions: which candidate was tested (ids into the mouse profile list).
    std::string candidate_profile_id;
    std::vector<Trial> trials;
    std::vector<RoundRecord> rounds;     // format 2
    std::vector<TrackSample> tracking;   // format 2, tracking rounds
};

inline constexpr const char* kMouseFitAlgorithmVersion = "mousefit-1.1";

}  // namespace fit
