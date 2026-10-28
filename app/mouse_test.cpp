#include "mouse_test.hpp"

#include <algorithm>
#include <cmath>

#include "fit/aim_report.hpp"
#include "fit/mouse_analysis.hpp"
#include "fit/stats.hpp"
#include "fit/timeutil.hpp"
#include "imgui.h"
#include "theme.hpp"
#include "widgets.hpp"

namespace fitapp {

using namespace fit;

namespace {
// Switching targets appear within this box around the round's centre (degrees).
constexpr double kSwitchYaw = 18.0, kSwitchPitch = 9.0;
// Tracking: the target wanders within this box at 8–30°/s, changing course every 0.6–1.4 s.
constexpr double kTrackYaw = 15.0, kTrackPitch = 8.0;
}  // namespace

MouseTest::MouseTest(IPlatform& platform, MouseTestConfig cfg)
    : platform_(platform), cfg_(std::move(cfg)), rng_(cfg_.plan.seed ^ uint64_t(unix_now())) {
    session_.id = make_id(cfg_.kind == SessionKind::Calibration ? "cal" : cfg_.kind == SessionKind::Check ? "chk" : "val");
    session_.kind = cfg_.kind;
    session_.format = 2;
    session_.started_utc = utc_now_iso();
    session_.algorithm_version = kMouseFitAlgorithmVersion;
    session_.seed = cfg_.plan.seed;
    session_.conditions = cfg_.conditions;
    session_.conditions.data_origin = cfg_.simulated_input ? "simulated" : "measured";
    session_.candidate_profile_id = cfg_.candidate_profile_id;
    cam_.hfov_deg_16x9 = cfg_.conditions.mapping.hfov_deg_16x9;
    // A plan without round information is played as flick rounds.
    while (cfg_.plan.block_info.size() < cfg_.plan.blocks.size()) {
        BlockInfo bi;
        const auto& b = cfg_.plan.blocks[cfg_.plan.block_info.size()];
        if (!b.empty()) bi.radius_deg = b.front().radius_deg;
        cfg_.plan.block_info.push_back(bi);
    }
    scored_total_ = cfg_.plan.scored_trials();
    first_round_ = cfg_.plan.warmup.empty() ? 0 : -1;
    locked_device_ = cfg_.locked_device_id;
}

MouseTest::~MouseTest() {
    if (capture_on_) end_capture();
}

const BlockInfo& MouseTest::info() const { return round_ < 0 ? cfg_.plan.warmup_info : cfg_.plan.block_info[size_t(round_)]; }
const std::vector<TargetSpec>& MouseTest::specs() const {
    return round_ < 0 ? cfg_.plan.warmup : cfg_.plan.blocks[size_t(round_)];
}

const Transform& MouseTest::active_transform() const {
    static const Transform identity;
    return condition_ == Condition::Candidate ? cfg_.candidate : identity;
}

void MouseTest::begin_capture() {
    std::string err;
    if (!platform_.mouse().start(&err)) {
        abort(("Raw mouse input could not be started: " + err).c_str());
        return;
    }
    platform_.set_cursor_captured(true);
    double fps = cfg_.limiter_fps;
    if (fps <= 0.0) {
        const double hz = platform_.frame_timing().refresh_hz;
        fps = std::clamp(hz > 0 ? hz * 2.0 : 480.0, 240.0, 1000.0);
    }
    platform_.set_render_mode(RenderMode::MouseTest, fps);
    capture_on_ = true;
    device_counter_at_start_ = platform_.mouse().device_change_counter();
    events_.clear();
    platform_.mouse().drain(events_);  // discard anything queued before the test started
    events_.clear();
    last_frame_us_ = 0;
}

void MouseTest::end_capture() {
    platform_.mouse().stop();
    platform_.set_cursor_captured(false);
    platform_.set_render_mode(RenderMode::Ui);
    capture_on_ = false;
}

void MouseTest::prepare_round(int index) {
    round_ = index;
    condition_ = index < 0 ? Condition::Baseline : cfg_.plan.block_condition[size_t(index)];
    transformer_.set(active_transform());
    round_started_ = false;
    spec_pos_ = 0;
    sw_.clear();
    sw_resolved_ = sw_spawned_ = 0;
    tk_played_us_ = 0;
    tk_on_s_ = tk_total_s_ = 0.0;
    hits_ = misses_ = 0;
    rr_ = RoundRecord{};
    rr_.index = index;
    rr_.task = info().task;
    rr_.condition = condition_;
    trial_active_ = false;
    phase_ = Phase::RoundIntro;
}

void MouseTest::abort(const char* why) {
    if (phase_ == Phase::Done || phase_ == Phase::Aborted) return;
    if (trial_active_ && (info().task == Task::Flick || info().task == Task::Micro))
        end_trial(TrialOutcome::Interrupted, platform_.mouse().now_us());
    trial_active_ = false;
    if (capture_on_) end_capture();
    session_.complete = false;
    session_.finished_utc = utc_now_iso();
    abort_reason_ = why ? why : "";
    phase_ = Phase::Aborted;
}

void MouseTest::on_focus_lost() {
    if (!capture_on_) return;
    if (trial_active_ && (info().task == Task::Flick || info().task == Task::Micro))
        end_trial(TrialOutcome::Interrupted, platform_.mouse().now_us());
    trial_active_ = false;  // a switching trial in progress is dropped; the round continues on resume
    end_capture();
    phase_ = Phase::Paused;
}

void MouseTest::begin() {
    if (phase_ != Phase::Instructions) return;
    prepare_round(first_round_);
    begin_capture();
}

void MouseTest::resume() {
    if (phase_ == Phase::RoundIntro) {
        start_requested_ = true;
        return;
    }
    if (phase_ != Phase::Paused) return;
    device_changed_ = false;
    begin_capture();
    if (phase_ != Phase::Aborted) phase_ = Phase::RoundIntro;  // click to continue the same round
}

MouseTestProbe MouseTest::probe() const {
    MouseTestProbe p;
    const bool playing = phase_ == Phase::Playing;
    p.task = round_ < round_count() ? info().task : Task::Flick;
    p.target_active = playing && trial_active_;
    p.spec = cur_.spec;
    p.target_counts = cur_.target_counts;
    p.t_appear_us = cur_.t_appear_us;
    p.trial_serial = trial_serial_;
    p.active_transform = active_transform();
    p.mapping = cfg_.conditions.mapping;
    p.yaw_deg = cam_.yaw_deg;
    p.pitch_deg = cam_.pitch_deg;
    if (playing && p.task == Task::Switch)
        for (const auto& t : sw_) p.switch_targets.push_back({t.yaw, t.pitch, info().radius_deg});
    if (playing && p.task == Task::Track) {
        p.tracking = true;
        p.track_yaw = tk_yaw_;
        p.track_pitch = tk_pitch_;
        p.track_vyaw = tk_vy_;
        p.track_vpitch = tk_vp_;
    }
    return p;
}

// ---- playing a round ----------------------------------------------------------------------------

void MouseTest::play(int64_t now) {
    start_requested_ = false;
    const Task task = info().task;
    if (!round_started_) {
        round_started_ = true;
        cam_.yaw_deg = cam_.pitch_deg = 0.0;
        anchor_yaw_ = anchor_pitch_ = 0.0;
        rr_.t_start_us = now;
        phase_ = Phase::Playing;
        if (task == Task::Track) {
            tk_yaw_ = tk_pitch_ = 0.0;
            tk_vy_ = tk_vp_ = 0.0;
            tk_next_turn_us_ = now;
            tk_last_us_ = now;
        } else if (task == Task::Switch) {
            const int start = std::min(info().visible_targets, info().kills);
            for (int i = 0; i < start; ++i) spawn_switch_target(now);
            begin_switch_trial(now);
        } else {
            spawn_single(now);
        }
        return;
    }
    // Continuing after a pause.
    phase_ = Phase::Playing;
    if (task == Task::Track) {
        tk_last_us_ = now;
    } else if (task == Task::Switch) {
        for (auto& t : sw_) t.spawn_us = now;  // fresh timers
        begin_switch_trial(now);
    } else {
        spawn_single(now);
    }
}

void MouseTest::spawn_single(int64_t now) {
    const auto& list = specs();
    while (spec_pos_ < list.size()) {
        TargetSpec spec = list[spec_pos_];
        spec.block = round_;
        if (spec.to_anchor) {  // back to the round's centre, from wherever the crosshair is
            const double dy = anchor_yaw_ - cam_.yaw_deg, dp = anchor_pitch_ - cam_.pitch_deg;
            spec.dist_deg = std::hypot(dy, dp);
            if (spec.dist_deg < 0.5) {  // already there: skip it
                ++spec_pos_;
                continue;
            }
            spec.dir_deg = std::fmod(std::atan2(dp, dy) * kRadToDeg + 360.0, 360.0);
        }
        const double th = spec.dir_deg * kDegToRad;
        const double dyaw = spec.dist_deg * std::cos(th);
        const double dpitch = spec.dist_deg * std::sin(th);
        const double k = cfg_.conditions.mapping.deg_per_count();
        target_yaw_ = cam_.yaw_deg + dyaw;
        target_pitch_ = cam_.pitch_deg + dpitch;
        cur_ = Trial{};
        cur_.spec = spec;
        cur_.target_counts = Vec2{dyaw / k, -dpitch / k};
        cur_.start_yaw_deg = cam_.yaw_deg;
        cur_.start_pitch_deg = cam_.pitch_deg;
        cur_.t_appear_us = now;
        cur_.timeout_ms = info().target_timeout_ms;
        trial_active_ = true;
        ++trial_serial_;
        return;
    }
    end_round(now);
}

void MouseTest::next_target(int64_t now) {
    if (phase_ != Phase::Playing) return;
    if (spec_pos_ >= specs().size()) end_round(now);
    else spawn_single(now);
}

void MouseTest::spawn_switch_target(int64_t now) {
    const double min_gap = 5.0, min_from_aim = 4.0;
    SwitchTarget t{anchor_yaw_, anchor_pitch_ + 5.0, now};
    for (int tries = 0; tries < 40; ++tries) {
        t.yaw = anchor_yaw_ + rng_.uniform(-kSwitchYaw, kSwitchYaw);
        t.pitch = anchor_pitch_ + rng_.uniform(-kSwitchPitch, kSwitchPitch);
        bool ok = angular_distance_deg(t.yaw, t.pitch, cam_.yaw_deg, cam_.pitch_deg) >= min_from_aim;
        for (const auto& o : sw_) ok = ok && angular_distance_deg(t.yaw, t.pitch, o.yaw, o.pitch) >= min_gap;
        if (ok) break;
    }
    sw_.push_back(t);
    ++sw_spawned_;
}

void MouseTest::begin_switch_trial(int64_t now) {
    cur_ = Trial{};
    cur_.spec.task = Task::Switch;
    cur_.spec.block = round_;
    cur_.spec.index = std::max(round_, 0) * 1000 + sw_resolved_;
    cur_.spec.condition = condition_;
    cur_.spec.purpose = round_ < 0 ? TrialPurpose::Warmup : TrialPurpose::Scored;
    cur_.spec.radius_deg = info().radius_deg;
    cur_.start_yaw_deg = cam_.yaw_deg;
    cur_.start_pitch_deg = cam_.pitch_deg;
    cur_.t_appear_us = now;
    cur_.timeout_ms = info().target_timeout_ms;
    trial_active_ = true;
    ++trial_serial_;
}

// A switching target was shot at (clicked) or expired. The trial so far (movement since the last
// shot) belongs to the target that was shot at: the one hit, or the nearest one for a miss.
void MouseTest::resolve_switch(int64_t t, bool clicked, bool hit, size_t which) {
    if (which >= sw_.size()) return;
    const SwitchTarget tg = sw_[which];
    if (clicked && trial_active_) {
        const double k = cfg_.conditions.mapping.deg_per_count();
        const double dyaw = tg.yaw - cur_.start_yaw_deg, dpitch = tg.pitch - cur_.start_pitch_deg;
        cur_.target_counts = Vec2{dyaw / k, -dpitch / k};
        cur_.spec.dist_deg = std::max(std::hypot(dyaw, dpitch), 0.01);
        cur_.spec.dir_deg = std::fmod(std::atan2(dpitch, dyaw) * kRadToDeg + 360.0, 360.0);
        cur_.spec.nominal_dist_deg = 0.0;
        target_yaw_ = tg.yaw;
        target_pitch_ = tg.pitch;
        ClickEvent c;
        c.t_us = t;
        c.offset_deg = angular_distance_deg(cam_.yaw_deg, cam_.pitch_deg, tg.yaw, tg.pitch);
        c.hit = hit;
        cur_.clicks.push_back(c);
        end_trial(hit ? TrialOutcome::Hit : TrialOutcome::Miss, t);
    } else if (!clicked) {
        ++rr_.timeouts;
        if (round_ >= 0) ++scored_done_;
        flash_hit_ = false;
        flash_yaw_ = tg.yaw;
        flash_pitch_ = tg.pitch;
        flash_radius_ = info().radius_deg;
        flash_until_us_ = t + 120000;
    }
    sw_.erase(sw_.begin() + long(which));
    ++sw_resolved_;
    if (sw_spawned_ < info().kills) spawn_switch_target(t);
    if (sw_resolved_ >= info().kills) {
        end_round(t);
        return;
    }
    if (clicked) begin_switch_trial(t);
}

void MouseTest::update_switch(int64_t now) {
    for (size_t i = 0; i < sw_.size() && phase_ == Phase::Playing;) {
        const int64_t expiry = sw_[i].spawn_us + int64_t(info().target_timeout_ms * 1000.0);
        if (now >= expiry) {
            resolve_switch(expiry, false, false, i);
            continue;  // the list changed; look at the same index again
        }
        ++i;
    }
}

void MouseTest::update_track(int64_t now) {
    const double dt = std::clamp(double(now - tk_last_us_) / 1e6, 0.0, 0.05);
    tk_last_us_ = now;
    if (now >= tk_next_turn_us_) {
        const double a = rng_.uniform(0.0, 2.0 * kPi), sp = rng_.uniform(8.0, 30.0);
        tk_vy_ = sp * std::cos(a);
        tk_vp_ = sp * std::sin(a);
        tk_next_turn_us_ = now + int64_t(rng_.uniform(0.6, 1.4) * 1e6);
    }
    if (std::fabs(tk_yaw_ + tk_vy_ * dt - anchor_yaw_) > kTrackYaw) tk_vy_ = -tk_vy_;
    if (std::fabs(tk_pitch_ + tk_vp_ * dt - anchor_pitch_) > kTrackPitch) tk_vp_ = -tk_vp_;
    tk_yaw_ += tk_vy_ * dt;
    tk_pitch_ += tk_vp_ * dt;
    const double r = info().radius_deg;
    const double e = angular_distance_deg(cam_.yaw_deg, cam_.pitch_deg, tk_yaw_, tk_pitch_);
    tk_total_s_ += dt;
    if (e <= r) tk_on_s_ += dt;
    if (round_ >= 0) {
        TrackSample s;
        s.t_us = now;
        s.round = round_;
        s.target_yaw = float(tk_yaw_);
        s.target_pitch = float(tk_pitch_);
        s.aim_yaw = float(cam_.yaw_deg);
        s.aim_pitch = float(cam_.pitch_deg);
        s.target_vyaw = float(tk_vy_);
        s.target_vpitch = float(tk_vp_);
        s.radius = float(r);
        session_.tracking.push_back(s);
    }
    tk_played_us_ += int64_t(dt * 1e6);
    if (double(tk_played_us_) >= info().duration_ms * 1000.0) end_round(now);
}

void MouseTest::end_trial(TrialOutcome outcome, int64_t t_end) {
    if (!trial_active_) return;
    cur_.outcome = outcome;
    cur_.t_end_us = t_end;
    while (!cur_.motion.empty() && cur_.motion.back().t_us > t_end) cur_.motion.pop_back();
    session_.trials.push_back(cur_);
    trial_active_ = false;
    if (outcome == TrialOutcome::Interrupted) return;
    if (cur_.spec.task == Task::Flick || cur_.spec.task == Task::Micro) ++spec_pos_;
    if (cur_.spec.purpose == TrialPurpose::Scored) ++scored_done_;
    if (outcome == TrialOutcome::Hit) {
        ++rr_.hits;
        ++hits_;
    } else if (outcome == TrialOutcome::Miss) {
        ++rr_.misses;
        ++misses_;
    } else {
        ++rr_.timeouts;
        ++misses_;
    }
    flash_hit_ = outcome == TrialOutcome::Hit;
    flash_yaw_ = target_yaw_;
    flash_pitch_ = target_pitch_;
    flash_radius_ = cur_.spec.radius_deg;
    flash_until_us_ = t_end + 120000;
}

void MouseTest::end_round(int64_t now) {
    trial_active_ = false;  // an unfinished switching trial has no target yet: dropped
    rr_.t_end_us = now;
    rr_.complete = true;
    if (round_ >= 0) session_.rounds.push_back(rr_);
    if (round_ + 1 < round_count()) prepare_round(round_ + 1);
    else finish();
}

void MouseTest::finish() {
    if (capture_on_) end_capture();
    session_.complete = true;
    session_.finished_utc = utc_now_iso();
    auto& c = session_.conditions;
    if (!frame_ms_.empty()) {
        const double med = median(frame_ms_);
        c.render_fps_median = med > 0 ? 1000.0 / med : 0.0;
        c.render_frame_p99_ms = percentile(frame_ms_, 99);
        c.render_hitches = int(std::count_if(frame_ms_.begin(), frame_ms_.end(), [&](double v) { return v > 3.0 * med && v > 4.0; }));
    }
    c.polling_hz_measured = estimate_polling_hz(session_.trials);
    c.tearing_allowed = platform_.frame_timing().tearing_supported;
    if (c.mouse_id.empty()) c.mouse_id = locked_device_;
    phase_ = Phase::Done;
}

void MouseTest::handle_event(const RawEvent& e) {
    if (e.injected) { ++ignored_injected_; return; }
    if (e.absolute) { ++ignored_absolute_; return; }
    const bool moved = e.dx != 0 || e.dy != 0;
    if (locked_device_.empty() && moved && !e.device_id.empty()) locked_device_ = e.device_id;
    if (!locked_device_.empty() && e.device_id != locked_device_) {
        if (moved || e.buttons) ++ignored_other_device_;
        return;
    }
    if (phase_ == Phase::RoundIntro) {
        if (e.buttons & kLeftDown) play(e.t_us);  // the starting click is not a shot
        return;
    }
    if (phase_ != Phase::Playing) return;

    const double k = cfg_.conditions.mapping.deg_per_count();
    const Vec2 out = transformer_.apply(Vec2{double(e.dx), double(e.dy)}, e.t_us);
    if (moved) {
        cam_.yaw_deg += out.x * k;
        cam_.pitch_deg = std::clamp(cam_.pitch_deg - out.y * k, -89.0, 89.0);
        if (trial_active_) cur_.motion.push_back({e.t_us, e.dx, e.dy, out.x, out.y});
    }
    if (!(e.buttons & kLeftDown)) return;
    const Task task = info().task;
    if (task == Task::Flick || task == Task::Micro) {
        if (!trial_active_) return;
        ClickEvent c;
        c.t_us = e.t_us;
        c.offset_deg = angular_distance_deg(cam_.yaw_deg, cam_.pitch_deg, target_yaw_, target_pitch_);
        c.hit = c.offset_deg <= cur_.spec.radius_deg;
        cur_.clicks.push_back(c);
        end_trial(c.hit ? TrialOutcome::Hit : TrialOutcome::Miss, e.t_us);  // a miss: the target disappears
        next_target(e.t_us);
    } else if (task == Task::Switch && !sw_.empty()) {
        size_t best = 0;
        double best_d = 1e9;
        for (size_t i = 0; i < sw_.size(); ++i) {
            const double d = angular_distance_deg(cam_.yaw_deg, cam_.pitch_deg, sw_[i].yaw, sw_[i].pitch);
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
        resolve_switch(e.t_us, true, best_d <= info().radius_deg, best);
    }
}

void MouseTest::process_events() {
    events_.clear();
    platform_.mouse().drain(events_);
    for (const auto& e : events_) {
        if (phase_ == Phase::Done || phase_ == Phase::Aborted || phase_ == Phase::Paused) break;
        handle_event(e);
    }
}

// ---- drawing ------------------------------------------------------------------------------------

void MouseTest::draw_overlay_card(ImVec2 origin, ImVec2 size, const char* title, const char* body, const char* primary,
                                  const char* secondary, bool* primary_clicked, bool* secondary_clicked) {
    const ImVec2 card(560, 0);
    ImGui::SetNextWindowPos(ImVec2(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(card, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::v4(theme::kCard, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, theme::v4(theme::kBorderHi));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 22));
    ImGui::Begin("##mousetest_overlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    ui::push_font(theme::fonts().semibold, theme::kH2);
    ImGui::TextUnformatted(title);
    ui::pop_font();
    ImGui::Dummy(ImVec2(0, 4));
    ui::wrapped(body, theme::kTextMuted);
    ImGui::Dummy(ImVec2(0, 8));
    if (primary && ui::button_primary(primary) && primary_clicked) *primary_clicked = true;
    if (secondary) {
        ImGui::SameLine();
        if (ui::button_secondary(secondary) && secondary_clicked) *secondary_clicked = true;
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

std::string MouseTest::round_title(int index) const {
    if (index < 0) return "Warm-up (not scored)";
    const Task t = cfg_.plan.block_info[size_t(index)].task;
    if (cfg_.kind == SessionKind::Validation) return ui::fmt("Block %d of %d", index + 1, round_count());
    if (cfg_.kind == SessionKind::Check) return ui::fmt("Quick check · round %d of %d", index + 1, round_count());
    return ui::fmt("Round %d of %d · %s", index + 1, round_count(), task_title(t));
}

std::string MouseTest::round_body(int index) const {
    const BlockInfo& bi = index < 0 ? cfg_.plan.warmup_info : cfg_.plan.block_info[size_t(index)];
    const double secs = bi.target_timeout_ms / 1000.0;
    switch (bi.task) {
        case Task::Flick:
            return ui::fmt("Shoot each target. The next one appears the moment you hit or miss, and each target "
                           "disappears after %.1f s. Flick at your normal in-game speed: quick but accurate.",
                           secs);
        case Task::Micro:
            return ui::fmt("Small, close targets: small, precise adjustments. Same rules: one shot each, %.1f s per "
                           "target.",
                           secs);
        case Task::Switch:
            return ui::fmt("%d targets at a time. Shoot them in any order; each one disappears after %.1f s and a new "
                           "one takes its place.",
                           bi.visible_targets, secs);
        case Task::Track:
            return ui::fmt("Keep the crosshair on the moving target for %.0f seconds. No clicking.", bi.duration_ms / 1000.0);
    }
    return "";
}

void MouseTest::draw_hud(ImDrawList* dl, ImVec2 o, ImVec2 s, int64_t now) {
    using namespace theme;
    double frac = scored_total_ ? double(scored_done_) / double(scored_total_) : 0.0;
    dl->AddRectFilled(o, ImVec2(o.x + s.x, o.y + 4), rgb(kSceneBar));
    dl->AddRectFilled(o, ImVec2(o.x + s.x * float(std::min(frac, 1.0)), o.y + 4), rgb(kSceneAccent));
    const char* name = cfg_.kind == SessionKind::Calibration ? "MouseFit calibration"
                       : cfg_.kind == SessionKind::Check  ? "Quick check (with your fix)"
                                                          : "Blinded comparison";
    ui::push_font(fonts().medium, kBody);
    const std::string title = std::string(name) + " · " + round_title(round_);
    dl->AddText(ImVec2(o.x + 24, o.y + 18), rgb(kSceneText), title.c_str());
    std::string line;
    if (phase_ == Phase::Playing && info().task == Task::Track) {
        const double left = std::max(0.0, info().duration_ms / 1000.0 - double(tk_played_us_) / 1e6);
        line = ui::fmt("%.0f s left · on target %.0f%%", std::ceil(left), tk_total_s_ > 0 ? 100.0 * tk_on_s_ / tk_total_s_ : 0.0);
    } else {
        line = ui::fmt("Hits %d · Misses %d", hits_, misses_);
        if (hits_ + misses_ > 0) line += ui::fmt(" · %.0f%%", 100.0 * hits_ / (hits_ + misses_));
    }
    dl->AddText(ImVec2(o.x + 24, o.y + 40), rgb(kSceneFaint), line.c_str());
    const char* esc = "Esc  stop test";
    const ImVec2 es = ImGui::CalcTextSize(esc);
    dl->AddText(ImVec2(o.x + s.x - es.x - 24, o.y + 18), rgb(kSceneMuted), esc);
    ui::pop_font();

    ui::push_font(fonts().mono, kSmall);
    double fps = 0.0;
    if (frame_ms_.size() > 30) {
        std::vector<double> tail(frame_ms_.end() - std::min<long>(240, long(frame_ms_.size())), frame_ms_.end());
        const double med = median(tail);
        fps = med > 0 ? 1000.0 / med : 0.0;
    }
    const std::string stats = fps > 0 ? ui::fmt("render %.0f fps", fps) : std::string("render —");
    dl->AddText(ImVec2(o.x + 24, o.y + s.y - 30), rgb(kSceneFaint), stats.c_str());
    std::string warn;
    if (ignored_injected_) warn += ui::fmt("ignored %d software-generated inputs  ", ignored_injected_);
    if (ignored_other_device_) warn += ui::fmt("ignored input from another mouse (%d)  ", ignored_other_device_);
    if (ignored_absolute_) warn += "absolute-position device ignored  ";
    if (!warn.empty()) dl->AddText(ImVec2(o.x + 200, o.y + s.y - 30), rgb(kSceneWarn), warn.c_str());
    ui::pop_font();

    if (cfg_.simulated_input) {
        ui::push_font(fonts().semibold, kBody);
        const char* t = "SIMULATED INPUT — development harness, not a measurement";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        const ImVec2 a(o.x + (s.x - ts.x) * 0.5f - 12, o.y + 64);
        dl->AddRectFilled(a, ImVec2(a.x + ts.x + 24, a.y + ts.y + 10), rgb(kSceneBad, 0.18f), 6);
        dl->AddRect(a, ImVec2(a.x + ts.x + 24, a.y + ts.y + 10), rgb(kSceneBad, 0.8f), 6);
        dl->AddText(ImVec2(a.x + 12, a.y + 5), rgb(kSceneBad), t);
        ui::pop_font();
    }
    (void)now;
}

void MouseTest::frame(ImVec2 origin, ImVec2 size) {
    const int64_t now = platform_.mouse().now_us();
    if (capture_on_ && phase_ == Phase::Playing) {
        if (last_frame_us_) frame_ms_.push_back(double(now - last_frame_us_) / 1000.0);
        last_frame_us_ = now;
    } else {
        last_frame_us_ = 0;
    }

    if (capture_on_ && !platform_.window_focused()) on_focus_lost();
    if (capture_on_ && platform_.mouse().device_change_counter() != device_counter_at_start_) {
        device_counter_at_start_ = platform_.mouse().device_change_counter();
        if (!locked_device_.empty() && !platform_.mouse().device(locked_device_)) {
            on_focus_lost();
            device_changed_ = true;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && running()) {
        abort("You stopped the test.");
        return;
    }
    if (capture_on_) process_events();
    if (phase_ == Phase::RoundIntro && capture_on_ && (start_requested_ || ImGui::IsKeyPressed(ImGuiKey_Space, false))) play(now);

    if (phase_ == Phase::Playing) {
        const Task task = info().task;
        if ((task == Task::Flick || task == Task::Micro) && trial_active_ &&
            double(now - cur_.t_appear_us) >= cur_.timeout_ms * 1000.0) {
            const int64_t t_end = cur_.t_appear_us + int64_t(cur_.timeout_ms * 1000.0);
            end_trial(TrialOutcome::Timeout, t_end);
            next_target(t_end);
        } else if (task == Task::Switch) {
            update_switch(now);
        } else if (task == Task::Track) {
            update_track(now);
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    SceneRenderer r;
    r.begin(dl, origin, size, cam_);
    r.draw_environment();
    if (phase_ == Phase::Playing) {
        const Task task = info().task;
        const double tmo_us = info().target_timeout_ms * 1000.0;
        if ((task == Task::Flick || task == Task::Micro) && trial_active_) {
            r.draw_target(target_yaw_, target_pitch_, cur_.spec.radius_deg, theme::rgb(theme::kTarget));
            r.draw_timer(target_yaw_, target_pitch_, cur_.spec.radius_deg, float(1.0 - double(now - cur_.t_appear_us) / tmo_us),
                         theme::rgb(theme::kSceneAccent, 0.9f));
        } else if (task == Task::Switch) {
            for (const auto& t : sw_) {
                r.draw_target(t.yaw, t.pitch, info().radius_deg, theme::rgb(theme::kTarget));
                r.draw_timer(t.yaw, t.pitch, info().radius_deg, float(1.0 - double(now - t.spawn_us) / tmo_us),
                             theme::rgb(theme::kSceneAccent, 0.9f));
            }
        } else if (task == Task::Track) {
            const bool on = angular_distance_deg(cam_.yaw_deg, cam_.pitch_deg, tk_yaw_, tk_pitch_) <= info().radius_deg;
            r.draw_target(tk_yaw_, tk_pitch_, info().radius_deg, theme::rgb(on ? theme::kSceneGood : theme::kTarget));
        }
    }
    if (now < flash_until_us_)
        r.draw_target(flash_yaw_, flash_pitch_, flash_radius_, theme::rgb(flash_hit_ ? theme::kSceneGood : theme::kSceneBad, 0.85f),
                      !flash_hit_);
    r.draw_crosshair(theme::rgb(theme::kCrosshair));
    draw_hud(dl, origin, size, now);

    bool p = false, q = false;
    switch (phase_) {
        case Phase::Instructions: {
            const bool cal = cfg_.kind == SessionKind::Calibration;
            const bool check = cfg_.kind == SessionKind::Check;
            int secs = 0;
            for (size_t i = 0; i < cfg_.plan.blocks.size(); ++i) {
                const BlockInfo& bi = cfg_.plan.block_info[i];
                secs += bi.task == Task::Track ? int(bi.duration_ms / 1000.0)
                        : bi.task == Task::Switch ? int(bi.kills * 1.0)
                                                  : int(double(cfg_.plan.blocks[i].size()) * 0.75);
                secs += 5;
            }
            const std::string length = secs >= 90 ? ui::fmt("about %d minutes", int(std::lround(secs / 60.0)))
                                                   : ui::fmt("about %d seconds", int(std::lround(secs / 10.0)) * 10);
            const std::string body =
                check ? ui::fmt("%d short rounds (%s) with your fix in use, to see whether it still fits. Click to start "
                                "each round. One shot per target; a miss makes it disappear. Esc stops; leaving the "
                                "window pauses.",
                                round_count(), length.c_str())
                : cal ? ui::fmt("%d short rounds (%s): flicks, micro-adjustments, tracking and target switching. Click to "
                                "start each round. One shot per target: a miss makes it disappear, and every target has a "
                                "timer. Play like you do in game. Esc stops; leaving the window pauses.",
                                round_count(), length.c_str())
                      : ui::fmt("%d blocks (%s). Some use your current setup and some the fix, in a hidden order. Play "
                                "every block the same way. A short warm-up comes first and is not scored.",
                                round_count(), length.c_str());
            draw_overlay_card(origin, size, cal ? "Calibration" : check ? "Quick check" : "Blinded comparison", body.c_str(),
                              "Start", "Cancel", &p, &q);
            if (p) begin();
            if (q) abort("Cancelled before starting.");
            break;
        }
        case Phase::RoundIntro: {
            const std::string body = round_started_ ? std::string("Click to continue this round.")
                                                    : round_body(round_) + "\n\nClick (or press Space) to start.";
            draw_overlay_card(origin, size, round_title(round_).c_str(), body.c_str(), nullptr, nullptr, nullptr, nullptr);
            break;
        }
        case Phase::Paused: {
            const char* body = device_changed_
                                   ? "The mouse used for this test was disconnected. Reconnect it and resume, or end the test."
                                   : "FitSuite lost focus, so mouse capture was released. The interrupted target will be asked "
                                     "again. Resume when you are ready.";
            draw_overlay_card(origin, size, "Test paused", body, "Resume", "End test", &p, &q);
            if (p) resume();
            if (q) abort("You ended the test while it was paused.");
            break;
        }
        default: break;
    }
}

}  // namespace fitapp
