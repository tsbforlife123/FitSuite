// MotionFit test runner: moving-detail identification and trail inspection.
//
// Refresh rates are switched by FitSuite itself (temporarily, reverted afterwards) without telling
// the player which rate is active, so those comparisons are blinded. Overdrive is changed by the
// player in the monitor menu, so those comparisons are not blinded and are labelled that way.
#pragma once

#include <string>
#include <vector>

#include "fit/motion.hpp"
#include "fit/profile.hpp"
#include "fit/rng.hpp"
#include "imgui.h"
#include "platform.hpp"

namespace fitapp {

struct MotionTestConfig {
    enum class Mode { Identification, Trails };
    Mode mode = Mode::Identification;
    std::vector<fit::MotionConfig> configs;
    fit::MotionPlan plan;               // identification
    int trail_rounds = 2;               // trails: each configuration is rated this many times
    std::string display_device;
    double original_refresh_hz = 0.0;
    double stimulus_ms = 450.0;
    double ring_size_frac = 0.05;       // ring diameter / screen height
    uint64_t seed = 5;
    bool simulated_input = false;
};

class MotionTest {
public:
    enum class Phase { Instructions, Switching, AwaitManual, Fixation, Stimulus, Respond, BlockBreak, Rate, Done, Aborted };

    MotionTest(IPlatform& platform, MotionTestConfig cfg);
    ~MotionTest();

    void frame(ImVec2 origin, ImVec2 size);
    void abort(const char* why);
    // The on-screen actions, also used by the development harness.
    void begin();
    void continue_manual();
    void continue_block();
    void rate(int trail, int overshoot);
    Phase phase() const { return phase_; }
    bool running() const { return phase_ != Phase::Done && phase_ != Phase::Aborted; }
    bool finished() const { return phase_ == Phase::Done; }
    bool aborted() const { return phase_ == Phase::Aborted; }
    const std::string& abort_reason() const { return abort_reason_; }
    fit::MotionSessionRecord take_record() { return std::move(rec_); }

    // Development harness: the simulated observer answers through this.
    bool awaiting_answer() const { return phase_ == Phase::Respond; }
    const fit::MotionTrialSpec* current_spec() const;
    void answer(int orientation);

private:
    void enter_block(int b);
    void apply_config(int c);
    void restore_display();
    void next_trial();
    void draw_card(ImVec2 o, ImVec2 s, const char* title, const char* body, const char* primary, const char* secondary,
                   bool* p, bool* q);
    void draw_ring(ImDrawList* dl, ImVec2 c, float diameter, int orientation, ImU32 col);
    void draw_trails(ImDrawList* dl, ImVec2 o, ImVec2 s, double t);

    IPlatform& platform_;
    MotionTestConfig cfg_;
    fit::MotionSessionRecord rec_;
    fit::Rng rng_;
    Phase phase_ = Phase::Instructions;
    std::string abort_reason_;

    int block_ = 0, trial_ = 0;
    int rate_index_ = 0;                // trails
    std::vector<int> rate_order_;
    int current_config_ = -1;
    bool switched_display_ = false;
    int64_t phase_t0_ = 0;
    int64_t stim_t0_ = 0;
    bool left_to_right_ = true;
    uint64_t dropped_at_start_ = 0;
    int trail_rating_ = -1, overshoot_rating_ = -1;
};

}  // namespace fitapp
