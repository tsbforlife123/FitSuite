#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "fit/timeutil.hpp"
#include "imgui.h"
#include "implot.h"

namespace fitapp {

using namespace fit;
using theme::fonts;

namespace {

ImPlotSpec spec_line(unsigned c, float w) {
    ImPlotSpec s;
    s.LineColor = theme::v4(c);
    s.LineWeight = w;
    return s;
}

ImPlotSpec spec_marker(unsigned c) {
    ImPlotSpec s;
    s.Marker = ImPlotMarker_Circle;
    s.MarkerSize = 5.0f;
    s.MarkerFillColor = theme::v4(c);
    s.MarkerLineColor = theme::v4(c);
    s.LineWeight = 0.0f;
    s.Flags = ImPlotItemFlags_NoLegend;
    return s;
}

double sustained_fps(const SetupProfile& s, std::string* from, ui::Origin* origin) {
    const FrameConfig* b = frame_baseline(s);
    if (!b || b->runs.empty()) return 0.0;
    std::vector<double> v;
    bool simulated = false;
    for (const auto& r : b->runs)
        if (std::isfinite(r.metrics.low1_fps)) {
            v.push_back(r.metrics.low1_fps);
            simulated = simulated || r.origin == "simulated";
        }
    if (v.empty()) return 0.0;
    if (from) *from = b->label;
    if (origin) *origin = simulated ? ui::Origin::Simulated : ui::Origin::Measured;
    return median(v);
}

void start_identification(App& app, std::vector<MotionConfig> configs) {
    MotionTestConfig cfg;
    cfg.mode = MotionTestConfig::Mode::Identification;
    cfg.configs = std::move(configs);
    MotionPlanOptions po;
    po.seed = uint64_t(unix_now());
    cfg.plan = make_motion_plan(int(cfg.configs.size()), po);
    cfg.display_device = app.setup().display.device;
    cfg.original_refresh_hz = app.setup().display.refresh_hz;
    app.start_motion_test(std::move(cfg));
}

void start_trails(App& app, std::vector<MotionConfig> configs) {
    MotionTestConfig cfg;
    cfg.mode = MotionTestConfig::Mode::Trails;
    cfg.configs = std::move(configs);
    cfg.display_device = app.setup().display.device;
    cfg.original_refresh_hz = app.setup().display.refresh_hz;
    cfg.seed = uint64_t(unix_now());
    app.start_motion_test(std::move(cfg));
}

// The setting a blinded identification test favours over the first one, or -1.
int favoured_config(const MotionSessionRecord& sel, bool blinded) {
    int best = -1;
    if (sel.test != "identification" || !blinded) return best;
    double best_th = -1.0;
    for (size_t c = 1; c < sel.configs.size(); ++c) {
        const auto cmp = compare_motion(sel.responses, 0, int(c));
        const auto f = fit_psychometric(sel.responses, int(c));
        if (cmp.verdict == "better" && f.ok && f.threshold_sw_s > best_th) {
            best = int(c);
            best_th = f.threshold_sw_s;
        }
    }
    return best;
}

bool session_blinded(const MotionSessionRecord& s) {
    return !s.configs.empty() && std::all_of(s.configs.begin(), s.configs.end(), [](const MotionConfig& c) { return c.app_controlled; });
}

// One button per tested setting: switch the refresh rate (with keep-or-revert) or record the choice.
void choose_setting(App& app, const MotionSessionRecord& sel_ref, bool blinded) {
    auto& setup = app.setup();
    const MotionSessionRecord* sel = &sel_ref;
    const int best = favoured_config(*sel, blinded);
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::TextUnformatted("Choose a setting:");
    for (size_t i = 0; i < sel->configs.size(); ++i) {
        const auto& c = sel->configs[i];
        ImGui::PushID(int(i));
        const bool current_rate = c.app_controlled && c.refresh_hz > 0 && std::fabs(c.refresh_hz - setup.display.refresh_hz) <= 0.5;
        const bool chosen = setup.motion_selected == c.label;
        auto record_choice = [&]() {
            setup.motion_selected = c.label;
            if (!c.overdrive.empty()) setup.display.overdrive = c.overdrive;
            app.save();
            app.toast(ui::Tone::Good, "Saved \"" + c.label + "\" as your monitor setting.");
        };
        if (c.app_controlled && c.refresh_hz > 0 && !current_rate) {
            const std::string label = ui::fmt("Switch Windows to %.0f Hz…", c.refresh_hz);
            if (int(i) == best ? ui::button_primary(label.c_str()) : ui::button_secondary(label.c_str()))
                switch_refresh_rate(app, setup.display.device, setup.display.refresh_hz, c.refresh_hz, c.label);
        } else {
            const std::string label = c.app_controlled ? ui::fmt("Keep %s (current)", c.label.c_str())
                                                       : ui::fmt("I set my monitor to \"%s\"", c.label.c_str());
            ImGui::BeginDisabled(chosen);
            if (int(i) == best ? ui::button_primary(label.c_str()) : ui::button_secondary(label.c_str())) record_choice();
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (chosen) ImGui::TextColored(theme::v4(theme::kAccent), "your setting");
        else if (int(i) == best) ImGui::TextColored(theme::v4(theme::kGood), "favoured by the blind test");
        else ui::faint(" ");
        ImGui::PopID();
    }
}

void results_card(App& app) {
    auto& setup = app.setup();
    if (setup.motion_sessions.empty()) return;
    ui::section("Results");
    const MotionSessionRecord* sel = &setup.motion_sessions.back();
    for (const auto& s : setup.motion_sessions)
        if (s.id == app.motion.last_session) sel = &s;
    ui::begin_card("##mo_res", 0, 0);
    ImGui::TextUnformatted("Session:");
    ImGui::SameLine();
    ui::origin_badge(origin_of(sel->origin));
    {
        const float row_h = ImGui::GetTextLineHeightWithSpacing();
        if (ImGui::BeginListBox("##mo_sessions", ImVec2(420, row_h * float(std::min<size_t>(setup.motion_sessions.size(), 4)) + 6))) {
            for (auto it = setup.motion_sessions.rbegin(); it != setup.motion_sessions.rend(); ++it)
                if (ImGui::Selectable(ui::fmt("%s · %s##%s", short_time(it->timestamp_utc).c_str(), it->test.c_str(), it->id.c_str()).c_str(),
                                      &*it == sel))
                    app.motion.last_session = it->id;
            ImGui::EndListBox();
        }
    }
    staleness_line(app, sel->conditions, "motionfit");
    const bool blinded = !sel->configs.empty() && std::all_of(sel->configs.begin(), sel->configs.end(), [](const MotionConfig& c) { return c.app_controlled; });
    ui::faint(blinded ? "Blinded: FitSuite switched the refresh rate without telling you which one was active."
                      : "Not blinded: you changed the monitor setting yourself and knew which one was active.");
    if (sel->test == "identification") {
        std::vector<PsychometricFit> fits;
        for (int c = 0; c < int(sel->configs.size()); ++c) fits.push_back(fit_psychometric(sel->responses, c));
        const float w = ImGui::GetContentRegionAvail().x;
        if (ImPlot::BeginPlot("Identification accuracy by speed", ImVec2(w * 0.6f, 300), ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
            ImPlot::SetupAxes("Speed (screen widths per second)", "Correct (%)", ImPlotAxisFlags_AutoFit, 0);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 105, ImPlotCond_Always);
            ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Outside | ImPlotLegendFlags_Horizontal);
            const unsigned cols[] = {theme::kAccent, theme::kOrange, theme::kInfo, theme::kBad};
            const double chance[2] = {25, 25};
            double cx[2] = {0.3, 3.0};
            ImPlot::PlotLine("Chance (25%)", cx, chance, 2, spec_line(theme::kBorderHi, 1.0f));
            for (size_t c = 0; c < fits.size(); ++c) {
                const auto& f = fits[c];
                std::vector<double> acc;
                for (double a : f.accuracy) acc.push_back(a * 100);
                ImPlot::PlotScatter(("##pts" + std::to_string(c)).c_str(), f.speeds.data(), acc.data(), int(acc.size()), spec_marker(cols[c % 4]));
                if (f.ok) {
                    std::vector<double> xs, ys;
                    for (int i = 0; i <= 60; ++i) {
                        const double v = 0.3 * std::pow(10.0, i / 60.0);
                        const double z = -(std::log2(v) - f.mu) / f.slope;
                        xs.push_back(v);
                        ys.push_back(100.0 * (0.25 + 0.75 * 0.98 / (1 + std::exp(-z))));
                    }
                    ImPlot::PlotLine(sel->configs[c].label.c_str(), xs.data(), ys.data(), int(xs.size()), spec_line(cols[c % 4], 2.0f));
                }
            }
            ImPlot::EndPlot();
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        for (size_t c = 0; c < fits.size(); ++c) {
            const auto& f = fits[c];
            double vmin = 0.0, vmax = 0.0;
            if (!f.speeds.empty()) {
                vmin = *std::min_element(f.speeds.begin(), f.speeds.end());
                vmax = *std::max_element(f.speeds.begin(), f.speeds.end());
            }
            const bool below = f.ok && f.threshold_sw_s < vmin, above = f.ok && f.threshold_sw_s > vmax;
            const std::string value = !f.ok ? "—" : below ? "< " + ui::num(vmin, 2) : above ? "> " + ui::num(vmax, 2)
                                                                                         : ui::num(f.threshold_sw_s, 2);
            ui::metric((sel->configs[c].label + " · details readable up to this speed (75% correct)").c_str(), value,
                       "screen widths/s", ui::Origin::None);
            if (below || above)
                ui::faint("Outside the tested speeds · %d answers · %.0f%% correct overall", f.n, f.overall_accuracy * 100);
            else if (f.ok)
                ui::faint("95%% range %.2f – %.2f · %d answers · %.0f%% correct overall", f.threshold_ci.lo, f.threshold_ci.hi,
                          f.n, f.overall_accuracy * 100);
            ImGui::Dummy(ImVec2(0, 4));
        }
        ImGui::EndGroup();
        for (size_t c = 1; c < sel->configs.size(); ++c) {
            const auto cmp = compare_motion(sel->responses, 0, int(c));
            ui::callout(cmp.verdict == "better" ? ui::Tone::Good : cmp.verdict == "worse" ? ui::Tone::Warn : ui::Tone::Neutral,
                        (sel->configs[c].label + " vs " + sel->configs[0].label).c_str(), cmp.explanation.c_str());
        }
        int dropped = 0;
        for (const auto& r : sel->responses) dropped += r.dropped_frames > 0 ? 1 : 0;
        if (dropped)
            ui::callout(ui::Tone::Warn, "Frames were dropped during some trials",
                        ui::fmt("%d trial(s) had presentation hitches, which make motion look worse than the monitor is. "
                                "Close other apps and repeat if results look odd.", dropped).c_str());
    } else {
        if (ImGui::BeginTable("##ratings", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            for (const char* h : {"Setting", "Ghosting (0–4)", "Overshoot halos (0–4)", "Ratings"}) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            for (int c = 0; c < int(sel->configs.size()); ++c) {
                std::vector<double> t, o;
                for (const auto& r : sel->ratings)
                    if (r.config == c) { t.push_back(r.trail); o.push_back(r.overshoot); }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(sel->configs[size_t(c)].label.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s", t.empty() ? "—" : ui::num(mean(t), 1).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s", o.empty() ? "—" : ui::num(mean(o), 1).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%zu", t.size());
            }
            ImGui::EndTable();
        }
        ui::origin_badge(ui::Origin::Preference);
        ImGui::SameLine();
        ui::faint("Your ratings of what you saw. Lower is cleaner. Software cannot measure the panel's pixel response.");
    }
    choose_setting(app, *sel, blinded);
    if (!blinded && sel->test == "identification")
        ui::faint("Not blinded, so expectations can colour the result. FitSuite does not pick a winner for you here.");
    ui::end_card();
}

}  // namespace

bool switch_refresh_rate(App& app, const std::string& device, double from_hz, double to_hz, const std::string& label) {
    if (app.motion.pending.active) return false;  // one change at a time
    auto& setup = app.setup();
    std::string err;
    AppliedChange ch;
    ch.id = make_id("ch");
    ch.kind = "display.refresh_rate";
    ch.target = device;
    ch.before = ui::fmt("%.3f", from_hz);
    ch.after = ui::fmt("%.3f", to_hz);
    ch.applied_utc = utc_now_iso();
    ch.status = "pending";
    if (!app.platform().set_refresh_persistent(device, to_hz, &err)) {
        app.toast(ui::Tone::Bad, "Windows refused the refresh rate change: " + err, 10);
        return false;
    }
    setup.applied_changes.push_back(ch);
    app.motion.pending = {true, device, from_hz, to_hz, app.time() + 15.0, ch.id, label};
    app.refresh_displays();
    app.save();
    return true;
}

void refresh_rate_notice(App& app) {
    for (const auto& s : app.slow_displays()) {
        const auto& d = s.display;
        ImGui::PushID(s.key.c_str());
        const std::string name = d.name.empty() ? std::string("Your monitor") : "Your monitor (" + d.name + ")";
        ui::callout(ui::Tone::Warn, "Your monitor is not running at full speed",
                    ui::fmt("%s runs at %.0f Hz, but Windows offers up to %.0f Hz at this resolution. A faster refresh "
                            "shows each frame sooner and makes moving targets smoother. This is a Windows setting, not a "
                            "VALORANT one.",
                            name.c_str(), d.refresh_hz, s.advice.max_hz)
                        .c_str());
        ImGui::BeginDisabled(app.motion.pending.active);
        if (ui::button_primary(ui::fmt("Switch to %.0f Hz", s.advice.max_hz).c_str()))
            switch_refresh_rate(app, d.device, d.refresh_hz, s.advice.max_hz, "");
        ImGui::EndDisabled();
        ImGui::SameLine(0, 14);
        ImGui::AlignTextToFramePadding();
        if (ui::button_link(ui::fmt("Keep %.0f Hz", d.refresh_hz).c_str())) app.keep_refresh(s.key);
        ImGui::SameLine(0, 14);
        ui::faint("You get 15 seconds to keep the new rate or go back.");
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PopID();
    }
}

// Confirms or reverts a persistent refresh-rate change (Windows-style 15 second safety net).
namespace {
void finish_pending_display(App& app, bool keep) {
    auto& p = app.motion.pending;
    std::string err;
    if (keep && !p.label.empty()) app.setup().motion_selected = p.label;
    for (auto& c : app.setup().applied_changes)
        if (c.id == p.change_id) {
            if (keep) c.status = "applied";
            else {
                app.platform().set_refresh_persistent(p.device, p.previous_hz, &err);
                c.status = "reverted";
                c.reverted_utc = utc_now_iso();
            }
        }
    p.active = false;
    app.refresh_hardware();
    app.save();
}
}  // namespace

void revert_pending_display(App& app) {
    if (app.motion.pending.active) finish_pending_display(app, false);
}

void tick_pending_display(App& app) {
    auto& p = app.motion.pending;
    if (!p.active) return;
    auto finish = [&](bool keep) { finish_pending_display(app, keep); };
    const double left = p.deadline - app.time();
    if (left <= 0) {
        finish(false);
        app.toast(ui::Tone::Info, "Refresh rate change reverted (not confirmed).");
        return;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460, 0));
    ImGui::Begin("Keep this refresh rate?", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    ui::wrapped(ui::fmt("The display now runs at %.0f Hz. If the picture is fine, keep it. Otherwise FitSuite returns to "
                        "%.0f Hz in %.0f seconds.", p.new_hz, p.previous_hz, std::ceil(left)).c_str());
    if (ui::button_primary("Keep")) finish(true);
    ImGui::SameLine();
    if (ui::button_secondary("Revert now")) finish(false);
    ImGui::End();
}

// Full MotionFit view with display details, every test and the psychometric charts (Nerd stats → MotionFit).
void motionfit_details(App& app) {
    auto& setup = app.setup();
    auto& m = app.motion;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float w = (avail - 14) * 0.5f;
    ui::begin_card("##mo_disp", w, 0);
    ui::card_title("Display", "Read from Windows.");
    ui::kv("Monitor", setup.display.name);
    ui::kv("Mode", setup.display.width ? ui::fmt("%d × %d at %.2f Hz", setup.display.width, setup.display.height, setup.display.refresh_hz) : "");
    std::string modes;
    for (double hz : setup.display.modes_hz) modes += (modes.empty() ? "" : ", ") + ui::fmt("%.0f", hz);
    ui::kv("Refresh rates", modes.empty() ? "unknown" : modes + " Hz");
    ui::kv("DDC/CI", setup.display.ddcci == "supported" ? "Supported (read-only in FitSuite)"
                     : setup.display.ddcci == "unsupported" ? "Not available" : "Not checked");
    if (setup.display.ddcci != "supported" && setup.display.ddcci != "unsupported" && !setup.display.device.empty()) {
        ImGui::SameLine();
        if (ui::button_link("check")) {
            setup.display.ddcci = app.platform().probe_ddcci(setup.display.device);
            app.save();
        }
        ImGui::SameLine();
        ui::help_marker("Asks the monitor whether it answers DDC/CI commands. This only reads; FitSuite never changes "
                        "monitor settings over DDC/CI. It can take a few seconds.");
    }
    ui::faint("Overdrive has no standard DDC/CI control, so you change it in the monitor's own menu.");
    char od[64];
    std::snprintf(od, sizeof od, "%s", setup.display.overdrive.c_str());
    ImGui::SetNextItemWidth(220);
    if (ImGui::InputTextWithHint("Current overdrive", "e.g. Normal, Fast, Extreme", od, sizeof od)) {
        setup.display.overdrive = od;
        app.save();
    }
    ui::end_card();
    ImGui::SameLine(0, 14);
    ui::begin_card("##mo_fps", w, 0);
    ui::card_title("Frame rate your PC sustains", "Motion looks different at the frame rate you actually get in game.");
    std::string from;
    ui::Origin fps_origin = ui::Origin::Measured;
    const double fps = sustained_fps(setup, &from, &fps_origin);
    if (fps > 0) {
        ui::metric("1% low in VALORANT (Frame rate tab)", ui::num(fps, 0), "FPS", fps_origin);
        ui::faint("From \"%s\".", from.c_str());
        ImGui::Checkbox("Show test motion at this frame rate", &m.match_framefit_fps);
        ui::help_marker("The moving target then updates at your in-game frame rate instead of every refresh, so "
                        "the comparison reflects what you see while playing (without variable refresh).");
    } else {
        ui::muted("No frame-rate runs yet. Record your current settings on the Frame rate tab to test at your real frame rate.");
        m.match_framefit_fps = false;
    }
    ui::end_card();

    ui::section("Tests");
    ui::begin_card("##mo_ref", w, 0);
    ui::card_title("Compare refresh rates (blinded)",
                   "FitSuite switches the refresh rate between blocks without telling you which one is active, then "
                   "restores your setting.");
    if (m.refresh_pick.size() != setup.display.modes_hz.size()) m.refresh_pick.assign(setup.display.modes_hz.size(), 0);
    int picked = 0;
    for (size_t i = 0; i < setup.display.modes_hz.size(); ++i) {
        bool b = m.refresh_pick[i] != 0;
        if (i) ImGui::SameLine();
        if (ImGui::Checkbox(ui::fmt("%.0f Hz##r%zu", setup.display.modes_hz[i], i).c_str(), &b)) m.refresh_pick[i] = b;
        picked += m.refresh_pick[i] ? 1 : 0;
    }
    if (setup.display.modes_hz.empty()) ui::faint("No refresh rates reported for this display.");
    ImGui::BeginDisabled(picked < 2);
    if (ui::button_primary("Start blinded test")) {
        std::vector<MotionConfig> cfgs;
        for (size_t i = 0; i < setup.display.modes_hz.size(); ++i)
            if (m.refresh_pick[i]) {
                MotionConfig c;
                c.refresh_hz = setup.display.modes_hz[i];
                c.label = ui::fmt("%.0f Hz", c.refresh_hz);
                c.app_controlled = true;
                c.overdrive = setup.display.overdrive;
                c.content_fps = m.match_framefit_fps ? fps : 0.0;
                if (c.content_fps > 0) c.label += ui::fmt(" · %.0f FPS content", c.content_fps);
                cfgs.push_back(c);
            }
        start_identification(app, cfgs);
    }
    ImGui::EndDisabled();
    {
        const MotionPlanOptions po;
        const int per_config = int(po.speeds_sw_s.size()) * po.reps_per_speed_per_block * po.blocks_per_config;
        const int n = std::max(picked, 2);
        const double minutes = (n * per_config * 2.0 + n * po.blocks_per_config * 6.0) / 60.0;  // ~2 s per answer + breaks
        ui::faint("About %.0f minutes for %d rates (%d answers each). Pick at least two rates.", std::ceil(minutes), n, per_config);
    }
    ui::end_card();
    ImGui::SameLine(0, 14);
    ui::begin_card("##mo_od", w, 0);
    ui::card_title("Compare overdrive settings (not blinded)",
                   "List the overdrive options to compare; FitSuite tells you when to switch in the monitor menu.");
    std::string drop;
    for (const auto& n : m.overdrive_names) {
        ImGui::PushID(n.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(n.c_str());
        ImGui::SameLine();
        if (ui::button_secondary("Remove")) drop = n;
        ImGui::SameLine(0, 20);
        ImGui::PopID();
    }
    if (!m.overdrive_names.empty()) ImGui::NewLine();
    if (!drop.empty()) m.overdrive_names.erase(std::find(m.overdrive_names.begin(), m.overdrive_names.end(), drop));
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##odname", "setting name, e.g. Normal", m.overdrive_input, sizeof m.overdrive_input);
    ImGui::SameLine();
    if (ui::button_secondary("Add") && m.overdrive_input[0]) {
        if (std::find(m.overdrive_names.begin(), m.overdrive_names.end(), m.overdrive_input) == m.overdrive_names.end())
            m.overdrive_names.push_back(m.overdrive_input);
        m.overdrive_input[0] = 0;
    }
    auto od_configs = [&]() {
        std::vector<MotionConfig> cfgs;
        for (const auto& n : m.overdrive_names) {
            MotionConfig c;
            c.label = "Overdrive " + n;
            c.overdrive = n;
            c.refresh_hz = setup.display.refresh_hz;
            c.content_fps = m.match_framefit_fps ? fps : 0.0;
            cfgs.push_back(c);
        }
        return cfgs;
    };
    ImGui::BeginDisabled(m.overdrive_names.size() < 2);
    if (ui::button_primary("Identification test")) start_identification(app, od_configs());
    ImGui::SameLine();
    if (ui::button_secondary("Trail inspection")) start_trails(app, od_configs());
    ImGui::EndDisabled();
    ui::faint("Each setting is tested in at least two separate blocks to reduce lucky results.");
    ui::end_card();

    results_card(app);
}

// ---- simple view ---------------------------------------------------------------------------------
void page_motionfit(App& app) {
    auto& setup = app.setup();
    auto& m = app.motion;
    ui::page_header("MotionFit", "Finds the monitor settings that let you see moving targets most clearly, tested blind on "
                                 "your own screen.");
    refresh_rate_notice(app);
    // Refresh rate
    ui::begin_card("##mos_rate", 0, 0);
    ui::card_title("Best refresh rate");
    ui::wrapped(ui::fmt("Your monitor runs at %.0f Hz now. FitSuite switches between the rates you pick without telling you which "
                        "is on, and checks how fast a target can move before you can no longer read it. Your setting comes back "
                        "afterwards.", setup.display.refresh_hz).c_str(), theme::kTextMuted);
    const auto& modes = setup.display.modes_hz;
    if (m.refresh_pick.size() != modes.size()) {
        m.refresh_pick.assign(modes.size(), 0);
        // Sensible default: the current rate and the highest one.
        for (size_t i = 0; i < modes.size(); ++i)
            if (std::fabs(modes[i] - setup.display.refresh_hz) < 0.5 || i + 1 == modes.size()) m.refresh_pick[i] = 1;
    }
    int picked = 0;
    for (size_t i = 0; i < modes.size(); ++i) {
        bool v = m.refresh_pick[i] != 0;
        if (i) ImGui::SameLine(0, 18);
        if (ImGui::Checkbox(ui::fmt("%.0f Hz##sr%zu", modes[i], i).c_str(), &v)) m.refresh_pick[i] = v ? 1 : 0;
        picked += m.refresh_pick[i];
    }
    ImGui::BeginDisabled(picked < 2);
    if (ui::button_primary("Start the blind test")) {
        std::vector<MotionConfig> cfgs;
        for (size_t i = 0; i < modes.size(); ++i)
            if (m.refresh_pick[i]) {
                MotionConfig c;
                c.refresh_hz = modes[i];
                c.label = ui::fmt("%.0f Hz", c.refresh_hz);
                c.app_controlled = true;
                c.overdrive = setup.display.overdrive;
                cfgs.push_back(c);
            }
        start_identification(app, cfgs);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    {
        const MotionPlanOptions po;
        const int per = int(po.speeds_sw_s.size()) * po.reps_per_speed_per_block * po.blocks_per_config;
        ui::faint("About %.0f minutes. Pick at least two rates.", std::ceil(std::max(picked, 2) * (per * 2.0 + po.blocks_per_config * 6.0) / 60.0));
    }
    // Latest blinded result
    const MotionSessionRecord* last = nullptr;
    for (const auto& sr : setup.motion_sessions)
        if (sr.test == "identification" && session_blinded(sr)) last = &sr;
    if (last) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Separator();
        const int best = favoured_config(*last, true);
        ui::push_font(fonts().semibold, theme::kBody);
        if (best >= 0) {
            const auto cmp = compare_motion(last->responses, 0, best);
            ImGui::TextColored(theme::v4(theme::kGood), "%s is clearly better than %s.", last->configs[size_t(best)].label.c_str(),
                               last->configs[0].label.c_str());
            ui::pop_font();
            ui::wrapped(cmp.explanation.c_str(), theme::kTextMuted);
        } else {
            ImGui::TextUnformatted("No clear difference between the rates you tested.");
            ui::pop_font();
            ui::faint("Keep the one you like; the extra refresh rate may not be worth it on this monitor for you.");
        }
        ImGui::SameLine();
        ui::origin_badge(origin_of(last->origin));
        choose_setting(app, *last, true);
    }
    ui::end_card();

    // Overdrive (optional)
    ui::begin_card("##mos_od", 0, 0);
    ui::card_title("Overdrive (optional)");
    ui::wrapped("Overdrive is a setting in your monitor's own menu (names like Normal, Fast, Extreme). Add the options you want "
                "to compare; FitSuite tells you when to switch. This one cannot be blind because you change it yourself.",
                theme::kTextMuted);
    std::string drop;
    for (const auto& n : m.overdrive_names) {
        ImGui::PushID(n.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(n.c_str());
        ImGui::SameLine();
        if (ui::button_secondary("Remove")) drop = n;
        ImGui::SameLine(0, 20);
        ImGui::PopID();
    }
    if (!m.overdrive_names.empty()) ImGui::NewLine();
    if (!drop.empty()) m.overdrive_names.erase(std::find(m.overdrive_names.begin(), m.overdrive_names.end(), drop));
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##sodname", "e.g. Normal", m.overdrive_input, sizeof m.overdrive_input);
    ImGui::SameLine();
    if (ui::button_secondary("Add") && m.overdrive_input[0]) {
        if (std::find(m.overdrive_names.begin(), m.overdrive_names.end(), m.overdrive_input) == m.overdrive_names.end())
            m.overdrive_names.push_back(m.overdrive_input);
        m.overdrive_input[0] = 0;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m.overdrive_names.size() < 2);
    if (ui::button_primary("Compare them")) {
        std::vector<MotionConfig> cfgs;
        for (const auto& n : m.overdrive_names) {
            MotionConfig c;
            c.label = "Overdrive " + n;
            c.overdrive = n;
            c.refresh_hz = setup.display.refresh_hz;
            cfgs.push_back(c);
        }
        start_identification(app, cfgs);
    }
    ImGui::EndDisabled();
    const MotionSessionRecord* od = nullptr;
    for (const auto& sr : setup.motion_sessions)
        if (!session_blinded(sr)) od = &sr;
    if (od) {
        ImGui::Dummy(ImVec2(0, 2));
        ui::faint("Last overdrive comparison: %s. Results are in Nerd stats → Monitor.", short_time(od->timestamp_utc).c_str());
        choose_setting(app, *od, false);
    }
    ui::end_card();
    ImGui::Dummy(ImVec2(0, 4));
    if (ui::button_link("All the numbers: Nerd stats → Monitor")) {
        app.nerd_tab = 2;
        app.go(Page::Nerd);
    }
}

}  // namespace fitapp
