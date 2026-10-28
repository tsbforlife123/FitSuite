#include "motion_test.hpp"

#include <algorithm>
#include <cmath>

#include "fit/timeutil.hpp"
#include "imgui.h"
#include "theme.hpp"
#include "widgets.hpp"

namespace fitapp {

using namespace fit;

MotionTest::MotionTest(IPlatform& platform, MotionTestConfig cfg)
    : platform_(platform), cfg_(std::move(cfg)), rng_(cfg_.seed ^ uint64_t(unix_now())) {
    rec_.id = make_id("motion");
    rec_.timestamp_utc = utc_now_iso();
    rec_.origin = cfg_.simulated_input ? "simulated" : "measured";
    rec_.test = cfg_.mode == MotionTestConfig::Mode::Identification ? "identification" : "trails";
    rec_.configs = cfg_.configs;
    if (cfg_.mode == MotionTestConfig::Mode::Trails) {
        const int n = int(cfg_.configs.size());
        for (int r = 0; r < std::max(cfg_.trail_rounds, 1); ++r) {
            std::vector<int> order(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) order[size_t(i)] = i;
            rng_.shuffle(order);
            if (!rate_order_.empty() && n > 1 && order.front() == rate_order_.back()) std::swap(order.front(), order.back());
            rate_order_.insert(rate_order_.end(), order.begin(), order.end());
        }
    }
}

MotionTest::~MotionTest() { restore_display(); }

void MotionTest::restore_display() {
    if (!switched_display_) return;
    std::string err;
    platform_.restore_display(cfg_.display_device, &err);
    switched_display_ = false;
}

void MotionTest::abort(const char* why) {
    if (!running()) return;
    restore_display();
    platform_.set_render_mode(RenderMode::Ui);
    abort_reason_ = why ? why : "";
    phase_ = Phase::Aborted;
}

const MotionTrialSpec* MotionTest::current_spec() const {
    if (cfg_.mode != MotionTestConfig::Mode::Identification) return nullptr;
    if (block_ >= int(cfg_.plan.blocks.size())) return nullptr;
    const auto& b = cfg_.plan.blocks[size_t(block_)];
    return trial_ < int(b.size()) ? &b[size_t(trial_)] : nullptr;
}

void MotionTest::apply_config(int c) {
    current_config_ = c;
    const MotionConfig& mc = cfg_.configs[size_t(c)];
    if (mc.app_controlled && mc.refresh_hz > 0) {
        std::string err;
        if (!platform_.set_refresh_temporary(cfg_.display_device, mc.refresh_hz, &err)) {
            abort(("Could not switch the refresh rate: " + err).c_str());
            return;
        }
        switched_display_ = true;
        phase_ = Phase::Switching;
    } else {
        phase_ = Phase::AwaitManual;
    }
    phase_t0_ = platform_.mouse().now_us();
}

void MotionTest::enter_block(int b) {
    block_ = b;
    trial_ = 0;
    if (cfg_.mode == MotionTestConfig::Mode::Identification) {
        if (b >= int(cfg_.plan.blocks.size())) {
            restore_display();
            platform_.set_render_mode(RenderMode::Ui);
            phase_ = Phase::Done;
            return;
        }
        apply_config(cfg_.plan.block_config[size_t(b)]);
    } else {
        if (rate_index_ >= int(rate_order_.size())) {
            restore_display();
            platform_.set_render_mode(RenderMode::Ui);
            phase_ = Phase::Done;
            return;
        }
        apply_config(rate_order_[size_t(rate_index_)]);
    }
}

void MotionTest::next_trial() {
    ++trial_;
    if (trial_ >= int(cfg_.plan.blocks[size_t(block_)].size())) {
        phase_ = Phase::BlockBreak;
        phase_t0_ = platform_.mouse().now_us();
        return;
    }
    phase_ = Phase::Fixation;
    phase_t0_ = platform_.mouse().now_us();
    left_to_right_ = rng_.below(2) == 0;
}

void MotionTest::begin() {
    if (phase_ != Phase::Instructions) return;
    platform_.set_render_mode(RenderMode::MotionTest);
    enter_block(0);
}

void MotionTest::continue_manual() {
    if (phase_ != Phase::AwaitManual) return;
    if (cfg_.mode == MotionTestConfig::Mode::Identification) {
        phase_ = Phase::Fixation;
        left_to_right_ = rng_.below(2) == 0;
    } else {
        phase_ = Phase::Rate;
        trail_rating_ = overshoot_rating_ = -1;
    }
    phase_t0_ = platform_.mouse().now_us();
}

void MotionTest::continue_block() {
    if (phase_ == Phase::BlockBreak) enter_block(block_ + 1);
}

void MotionTest::rate(int trail, int overshoot) {
    if (phase_ != Phase::Rate || trail < 0 || overshoot < 0) return;
    rec_.ratings.push_back({current_config_, rate_index_ / std::max(1, int(cfg_.configs.size())), trail, overshoot});
    ++rate_index_;
    enter_block(0);
}

void MotionTest::answer(int orientation) {
    const MotionTrialSpec* s = current_spec();
    if (!s || phase_ != Phase::Respond) return;
    MotionResponse r;
    r.spec = *s;
    r.answer = orientation;
    r.correct = orientation == s->orientation;
    r.response_ms = double(platform_.mouse().now_us() - phase_t0_) / 1000.0;
    r.dropped_frames = int(platform_.frame_timing().dropped_frames - dropped_at_start_);
    rec_.responses.push_back(r);
    next_trial();
}

void MotionTest::draw_card(ImVec2 o, ImVec2 s, const char* title, const char* body, const char* primary, const char* secondary,
                           bool* p, bool* q) {
    ImGui::SetNextWindowPos(ImVec2(o.x + s.x * 0.5f, o.y + s.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(580, 0), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::v4(theme::kCard, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, theme::v4(theme::kBorderHi));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 22));
    ImGui::Begin("##motiontest_overlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    ui::push_font(theme::fonts().semibold, theme::kH2);
    ImGui::TextUnformatted(title);
    ui::pop_font();
    ImGui::Dummy(ImVec2(0, 4));
    ui::wrapped(body, theme::kTextMuted);
    ImGui::Dummy(ImVec2(0, 8));
    if (primary && ui::button_primary(primary) && p) *p = true;
    if (secondary) {
        ImGui::SameLine();
        if (ui::button_secondary(secondary) && q) *q = true;
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void MotionTest::draw_ring(ImDrawList* dl, ImVec2 c, float d, int orientation, ImU32 col) {
    const float stroke = d / 5.0f;
    const float r = (d - stroke) * 0.5f;
    // Screen angles (y down): right 0, up -90°, left 180°, down 90°.
    static const float dirs[4] = {0.0f, -1.5707963f, 3.1415927f, 1.5707963f};
    const float gap = (d / 5.0f) / r;  // gap width = stroke width (Landolt C)
    const float a0 = dirs[orientation & 3] + gap * 0.5f;
    const float a1 = dirs[orientation & 3] + 6.2831853f - gap * 0.5f;
    dl->PathArcTo(c, r, a0, a1, 64);
    dl->PathStroke(col, stroke);
}

void MotionTest::draw_trails(ImDrawList* dl, ImVec2 o, ImVec2 s, double t) {
    // Three lanes on dark, mid and light grey; ghosting shows as smearing behind the moving block and
    // overshoot (inverse ghosting) as a bright or dark halo. A static copy is drawn for reference.
    const unsigned bg[3] = {0x262626, 0x7F7F7F, 0xD0D0D0};
    const unsigned fg[3] = {0xFFFFFF, 0x2A2A2A, 0x101010};
    const float lane_h = s.y * 0.22f;
    const float top = o.y + s.y * 0.17f;
    const float sq = lane_h * 0.42f;
    const double speed = 0.5 * s.x;  // half a screen width per second
    for (int i = 0; i < 3; ++i) {
        const float y0 = top + i * (lane_h + 8);
        dl->AddRectFilled(ImVec2(o.x, y0), ImVec2(o.x + s.x, y0 + lane_h), theme::rgb(bg[i]));
        double hz = platform_.frame_timing().refresh_hz;
        const MotionConfig* mc = current_config_ >= 0 ? &cfg_.configs[size_t(current_config_)] : nullptr;
        double step = hz > 0 ? 1.0 / hz : 0.0;
        if (mc && mc->content_fps > 0 && (hz <= 0 || mc->content_fps < hz)) step = 1.0 / mc->content_fps;
        const double ts = step > 0 ? std::floor(t / step) * step : t;
        const double span = s.x + sq * 2;
        const float x = float(o.x - sq + std::fmod(ts * speed, span));
        const float yc = y0 + lane_h * 0.5f;
        dl->AddRectFilled(ImVec2(x, yc - sq * 0.5f), ImVec2(x + sq, yc + sq * 0.5f), theme::rgb(fg[i]));
        dl->AddRectFilled(ImVec2(o.x + 24, yc - sq * 0.5f), ImVec2(o.x + 24 + sq, yc + sq * 0.5f), theme::rgb(fg[i]));
    }
}

void MotionTest::frame(ImVec2 o, ImVec2 s) {
    using namespace theme;
    const int64_t now = platform_.mouse().now_us();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + s.x, o.y + s.y), rgb(0x1A1D21));
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && running()) {
        abort("You stopped the test.");
        return;
    }
    if (!platform_.window_focused() && running() && phase_ != Phase::Instructions) {
        abort("FitSuite lost focus during the motion test.");
        return;
    }
    const bool ident = cfg_.mode == MotionTestConfig::Mode::Identification;
    if (cfg_.simulated_input) {
        ui::push_font(fonts().semibold, kBody);
        const char* t = "SIMULATED OBSERVER — development harness, not a measurement";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(o.x + (s.x - ts.x) * 0.5f, o.y + 20), rgb(kSceneBad), t);
        ui::pop_font();
    }
    // Progress.
    {
        float frac = 0.0f;
        if (ident) {
            size_t total = 0, done = rec_.responses.size();
            for (const auto& b : cfg_.plan.blocks) total += b.size();
            frac = total ? float(done) / float(total) : 0.0f;
        } else {
            frac = rate_order_.empty() ? 0.0f : float(rate_index_) / float(rate_order_.size());
        }
        dl->AddRectFilled(o, ImVec2(o.x + s.x, o.y + 4), rgb(kSceneBar));
        dl->AddRectFilled(o, ImVec2(o.x + s.x * frac, o.y + 4), rgb(kSceneAccent));
        ui::push_font(fonts().medium, kBody);
        dl->AddText(ImVec2(o.x + s.x - 140, o.y + 18), rgb(kSceneMuted), "Esc  stop test");
        ui::pop_font();
    }

    bool p = false, q = false;
    switch (phase_) {
        case Phase::Instructions: {
            const char* body = ident
                ? "A ring with a gap moves across the screen. Follow it with your eyes and report where the gap points "
                  "with the arrow keys (or W A S D). If you are not sure, take your best guess. The display settings change "
                  "between blocks; refresh-rate changes are hidden from you."
                : "Moving blocks cross three grey lanes. Look for dark smearing behind them (ghosting) and bright or dark "
                  "halos (overshoot / inverse ghosting), then rate what you see. Each setting is shown more than once.";
            draw_card(o, s, ident ? "Motion clarity test" : "Trail inspection", body, "Start", "Cancel", &p, &q);
            if (p) begin();
            if (q) abort("Cancelled before starting.");
            break;
        }
        case Phase::Switching: {
            ui::push_font(fonts().medium, kH3);
            const char* t = "Preparing the display…";
            const ImVec2 ts = ImGui::CalcTextSize(t);
            dl->AddText(ImVec2(o.x + (s.x - ts.x) * 0.5f, o.y + s.y * 0.5f), rgb(kSceneMuted), t);
            ui::pop_font();
            if (now - phase_t0_ > 1800000) {  // monitors need a moment to settle after a mode change
                if (ident) {
                    phase_ = Phase::Fixation;
                    phase_t0_ = now;
                    left_to_right_ = rng_.below(2) == 0;
                } else {
                    phase_ = Phase::Rate;
                    phase_t0_ = now;
                    trail_rating_ = overshoot_rating_ = -1;
                }
            }
            break;
        }
        case Phase::AwaitManual: {
            const MotionConfig& mc = cfg_.configs[size_t(current_config_)];
            const std::string body = ui::fmt("Use your monitor's on-screen menu to set: %s. Then press Continue. "
                                             "(FitSuite cannot change this setting itself.)",
                                             mc.overdrive.empty() ? mc.label.c_str() : mc.overdrive.c_str());
            draw_card(o, s, "Change the monitor setting", body.c_str(), "Continue", "Stop test", &p, &q);
            if (p) continue_manual();
            if (q) abort("You stopped the test.");
            break;
        }
        case Phase::Fixation:
        case Phase::Stimulus: {
            const MotionTrialSpec* sp = current_spec();
            if (!sp) break;
            const float d = float(cfg_.ring_size_frac * s.y);
            const double speed_px = sp->speed_sw_s * s.x;
            const double dist = speed_px * cfg_.stimulus_ms / 1000.0;
            const double x_start = o.x + (s.x - dist) * 0.5;
            const float yc = o.y + s.y * 0.5f;
            if (phase_ == Phase::Fixation) {
                const float x = float(left_to_right_ ? x_start : x_start + dist);
                dl->AddCircleFilled(ImVec2(x, yc), 3.0f, rgb(0xD8DDE3));
                if (now - phase_t0_ > 600000) {
                    phase_ = Phase::Stimulus;
                    stim_t0_ = now;
                    dropped_at_start_ = platform_.frame_timing().dropped_frames;
                }
                break;
            }
            double t = double(now - stim_t0_) / 1e6;
            const double hz = platform_.frame_timing().refresh_hz;
            const MotionConfig& mc = cfg_.configs[size_t(sp->config)];
            double step = hz > 0 ? 1.0 / hz : 0.0;
            if (mc.content_fps > 0 && (hz <= 0 || mc.content_fps < hz)) step = 1.0 / mc.content_fps;
            if (step > 0) t = std::round(t / step) * step;  // one position per displayed frame
            if (t * 1000.0 >= cfg_.stimulus_ms) {
                phase_ = Phase::Respond;
                phase_t0_ = now;
                break;
            }
            const double off = speed_px * t;
            const float x = float(left_to_right_ ? x_start + off : x_start + dist - off);
            draw_ring(dl, ImVec2(x, yc), d, sp->orientation, rgb(0xD8DDE3));
            break;
        }
        case Phase::Respond: {
            ui::push_font(fonts().medium, kH3);
            const char* t = "Where did the gap point?   ←  ↑  →  ↓";
            const ImVec2 ts = ImGui::CalcTextSize(t);
            dl->AddText(ImVec2(o.x + (s.x - ts.x) * 0.5f, o.y + s.y * 0.5f - 10), rgb(kSceneText), t);
            ui::pop_font();
            int a = -1;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) || ImGui::IsKeyPressed(ImGuiKey_D, false)) a = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) || ImGui::IsKeyPressed(ImGuiKey_W, false)) a = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) || ImGui::IsKeyPressed(ImGuiKey_A, false)) a = 2;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) || ImGui::IsKeyPressed(ImGuiKey_S, false)) a = 3;
            if (a >= 0) answer(a);
            break;
        }
        case Phase::BlockBreak: {
            const std::string title = ui::fmt("Block %d of %d complete", block_ + 1, int(cfg_.plan.blocks.size()));
            draw_card(o, s, title.c_str(), "Rest your eyes for a moment, then continue.", "Continue", nullptr, &p, nullptr);
            if (p || ImGui::IsKeyPressed(ImGuiKey_Space, false)) continue_block();
            break;
        }
        case Phase::Rate: {
            draw_trails(dl, o, s, double(now - phase_t0_) / 1e6);
            const float panel_y = o.y + s.y * 0.17f + 3 * (s.y * 0.22f + 8) + 12;
            ImGui::SetNextWindowPos(ImVec2(o.x + s.x * 0.5f, panel_y), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, v4(kCard, 0.96f));
            ImGui::Begin("##rate", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
            static const char* labels[5] = {"None", "Faint", "Visible", "Strong", "Severe"};
            ImGui::TextUnformatted("Dark smearing behind the blocks (ghosting)");
            for (int i = 0; i < 5; ++i) {
                if (i) ImGui::SameLine();
                if (ImGui::RadioButton(ui::fmt("%s##t%d", labels[i], i).c_str(), trail_rating_ == i)) trail_rating_ = i;
            }
            ImGui::TextUnformatted("Bright or dark halos (overshoot / inverse ghosting)");
            for (int i = 0; i < 5; ++i) {
                if (i) ImGui::SameLine();
                if (ImGui::RadioButton(ui::fmt("%s##o%d", labels[i], i).c_str(), overshoot_rating_ == i)) overshoot_rating_ = i;
            }
            ImGui::BeginDisabled(trail_rating_ < 0 || overshoot_rating_ < 0);
            if (ui::button_primary("Next")) rate(trail_rating_, overshoot_rating_);
            ImGui::EndDisabled();
            ImGui::End();
            ImGui::PopStyleColor();
            break;
        }
        default: break;
    }
}

}  // namespace fitapp
