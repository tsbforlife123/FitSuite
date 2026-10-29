// MouseFit test runner: calibration, blinded validation and quick checks in the FPS-style scene.
//
// Sessions are played in rounds, like an aim trainer. Each round starts with a click; after that a
// new target appears the moment the last one was hit, missed (it disappears) or ran out of time
// (every target has its own timer). Kinds of round: flicks and micro-adjustments (one target at a
// time, alternating out from the round's centre and back), target switching (several targets at
// once) and tracking (keep the crosshair on a moving target).
//
// Raw input is captured only while a test is running and the window has focus. Esc ends the test
// at once; losing focus pauses it, releases the mouse and asks the interrupted target again.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "fit/mouse_schedule.hpp"
#include "fit/mouse_transform.hpp"
#include "fit/mouse_types.hpp"
#include "fit/rng.hpp"
#include "platform.hpp"
#include "scene.hpp"

namespace fitapp {

struct MouseTestConfig {
    fit::SessionKind kind = fit::SessionKind::Calibration;
    fit::Plan plan;                    // round-based (format 2); a format-1 plan is played as flick rounds
    fit::Transform candidate;          // validation and quick check: applied in candidate blocks
    std::string candidate_profile_id;
    fit::SessionConditions conditions; // mapping, DPI, mouse, display
    std::string locked_device_id;      // empty: lock to the first mouse that moves
    double limiter_fps = 0.0;          // 0: twice the refresh rate (max 1000)
    bool simulated_input = false;      // development harness only; results are labelled "simulated"
};

// Read-only view used by the development harness to drive its simulated player.
struct MouseTestProbe {
    bool target_active = false;        // a flick, micro or switching target is waiting to be shot
    fit::Task task = fit::Task::Flick;
    fit::TargetSpec spec;
    fit::Vec2 target_counts;           // from the crosshair at the trial's start
    int64_t t_appear_us = 0;
    uint64_t trial_serial = 0;
    fit::Transform active_transform;
    fit::GameMapping mapping;
    double yaw_deg = 0.0, pitch_deg = 0.0;
    struct Point {
        double yaw, pitch, radius;
    };
    std::vector<Point> switch_targets; // switching round: the targets on screen
    bool tracking = false;             // a tracking round is being played
    double track_yaw = 0, track_pitch = 0, track_vyaw = 0, track_vpitch = 0;
};

class MouseTest {
public:
    enum class Phase { Instructions, RoundIntro, Playing, Paused, Done, Aborted };

    MouseTest(IPlatform& platform, MouseTestConfig cfg);
    ~MouseTest();

    // Advances the test and draws the full-screen scene plus HUD into the current ImGui window.
    void frame(ImVec2 origin, ImVec2 size);
    void on_focus_lost();
    void abort(const char* why);
    // Leaves the instructions screen and starts capturing (same as pressing Start).
    void begin();
    // Starts the round shown on the round screen, or resumes after a pause (same as clicking).
    void resume();

    Phase phase() const { return phase_; }
    bool finished() const { return phase_ == Phase::Done; }
    bool aborted() const { return phase_ == Phase::Aborted; }
    bool running() const { return phase_ != Phase::Done && phase_ != Phase::Aborted; }
    const fit::Session& session() const { return session_; }
    fit::Session take_session() { return std::move(session_); }
    const std::string& abort_reason() const { return abort_reason_; }
    MouseTestProbe probe() const;

private:
    struct SwitchTarget {
        double yaw, pitch;
        int64_t spawn_us;
    };

    void begin_capture();
    void end_capture();
    const fit::BlockInfo& info() const;
    const std::vector<fit::TargetSpec>& specs() const;
    int round_count() const { return int(cfg_.plan.blocks.size()); }
    void prepare_round(int index);  // shows the round screen
    void play(int64_t now);         // the click that starts (or continues) the round
    void next_target(int64_t now);
    void spawn_single(int64_t now);
    void spawn_switch_target(int64_t now);
    void begin_switch_trial(int64_t now);
    void resolve_switch(int64_t t, bool clicked, bool hit_any, size_t which);
    void update_switch(int64_t now);
    void update_track(int64_t now);
    void end_trial(fit::TrialOutcome outcome, int64_t t_end);
    void end_round(int64_t now);
    void finish();
    void process_events();
    void handle_event(const RawEvent& e);
    void draw_hud(ImDrawList* dl, ImVec2 origin, ImVec2 size, int64_t now);
    void draw_overlay_card(ImVec2 origin, ImVec2 size, const char* title, const char* body, const char* primary,
                           const char* secondary, bool* primary_clicked, bool* secondary_clicked);
    std::string round_title(int index) const;
    std::string round_body(int index) const;
    bool returning_round(int index) const;  // calibration flick round after the first: "Returning flicks"
    const fit::Transform& active_transform() const;

    IPlatform& platform_;
    MouseTestConfig cfg_;
    fit::Session session_;
    fit::Rng rng_;
    Phase phase_ = Phase::Instructions;
    std::string abort_reason_;

    // Rounds: index -1 is the (unscored) warm-up when the plan has one.
    int round_ = 0;
    int first_round_ = 0;
    bool round_started_ = false;  // false: the round screen is waiting for the first click
    size_t spec_pos_ = 0;         // flick / micro: next target in the round
    fit::Condition condition_ = fit::Condition::Baseline;
    fit::RoundRecord rr_;
    double anchor_yaw_ = 0.0, anchor_pitch_ = 0.0;

    Camera cam_;
    fit::Transformer transformer_;
    std::vector<RawEvent> events_;

    bool trial_active_ = false;
    fit::Trial cur_;
    double target_yaw_ = 0.0, target_pitch_ = 0.0;
    uint64_t trial_serial_ = 0;
    int64_t flash_until_us_ = 0;
    bool flash_hit_ = false;
    double flash_yaw_ = 0.0, flash_pitch_ = 0.0, flash_radius_ = 1.0;

    // Switching rounds.
    std::vector<SwitchTarget> sw_;
    int sw_resolved_ = 0, sw_spawned_ = 0;

    // Tracking rounds.
    double tk_yaw_ = 0, tk_pitch_ = 0, tk_vy_ = 0, tk_vp_ = 0;
    fit::TrackMotion tk_motion_;
    struct TrailPoint {
        int64_t t_us;
        double yaw, pitch;
    };
    std::vector<TrailPoint> tk_trail_;  // where the target was over the last fit::kTrackTrailMs
    std::vector<TrailPoint> aim_trail_; // where the crosshair pointed recently (drawn as its trail)
    bool track_on_target() const;       // on the target or its trail
    int64_t tk_last_us_ = 0, tk_played_us_ = 0;
    double tk_on_s_ = 0.0, tk_total_s_ = 0.0;

    size_t scored_done_ = 0, scored_total_ = 0;
    int hits_ = 0, misses_ = 0;  // this round, for the HUD
    int ignored_injected_ = 0, ignored_other_device_ = 0, ignored_absolute_ = 0;
    std::string locked_device_;
    uint64_t device_counter_at_start_ = 0;
    bool device_changed_ = false;

    int64_t last_frame_us_ = 0;
    std::vector<double> frame_ms_;
    bool capture_on_ = false;
    bool start_requested_ = false;
};

}  // namespace fitapp
