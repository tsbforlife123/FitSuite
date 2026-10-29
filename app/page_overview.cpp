#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "fit/motion.hpp"
#include "fit/mouse_fit.hpp"
#include "imgui.h"

namespace fitapp {

using namespace fit;
using theme::fonts;

namespace {

struct ToolStatus {
    std::vector<std::pair<std::string, ui::Origin>> measured;
    std::vector<std::string> recommended;
    std::vector<std::string> needs;
};

ToolStatus mouse_status(App& app) {
    ToolStatus s;
    auto& setup = app.setup();
    if (app.mouse.calibration) {
        const Session& c = *app.mouse.calibration;
        size_t n = 0;
        for (const auto& t : c.trials) n += t.spec.purpose == TrialPurpose::Scored ? 1 : 0;
        std::string line = ui::fmt("Calibration %s · %zu flicks", short_time(c.finished_utc).c_str(), n);
        if (app.mouse.fit && app.mouse.fit->ok)
            line += ui::fmt(" · largest bias %.1f%% distance, %.1f° angle", app.mouse.fit->max_radial_bias_before_pct,
                            app.mouse.fit->max_angle_bias_before_deg);
        s.measured.push_back({line, origin_of(c.conditions.data_origin)});
        Conditions recorded;
        for (const auto& m : setup.mouse_profiles)
            if (m.calibration_session == c.id) recorded = m.conditions;
        if (recorded.empty()) recorded = conditions_for("mousefit", setup);
    } else {
        s.needs.push_back("Run a calibration (about 4 minutes) to measure your directional aim.");
    }
    const MouseProfileRecord* active = nullptr;
    for (const auto& m : setup.mouse_profiles)
        if (m.id == setup.active_mouse_profile) active = &m;
    if (active && active->validation && active->validation->verdict == "improved")
        s.recommended.push_back(ui::fmt("Use \"%s\" — validated %.1f%% faster aim", active->label.c_str(),
                                        (1.0 - active->validation->time_ratio) * 100.0));
    else
        s.recommended.push_back("Keep your current mouse setup (no validated correction yet).");
    for (const auto& m : setup.mouse_profiles) {
        if (m.status == "discarded") continue;
        if (!m.validation)
            s.needs.push_back("\"" + m.label + "\" is a candidate: run a blinded comparison before using it.");
        const Staleness st = check_staleness(m.conditions, conditions_for("mousefit", setup));
        if (st.outdated) s.needs.push_back("\"" + m.label + "\" may be outdated: " + st.reasons.front() + ".");
    }
    return s;
}

ToolStatus frame_status(App& app) {
    ToolStatus s;
    auto& setup = app.setup();
    int runs = 0, configs = 0;
    for (const auto& c : setup.frame_configs) {
        runs += int(c.runs.size());
        configs += c.runs.empty() ? 0 : 1;
        if (!c.runs.empty() && c.runs.size() < 3)
            s.needs.push_back(ui::fmt("\"%s\": %zu run(s); record at least 3 for a reliable comparison.", c.label.c_str(),
                                      c.runs.size()));
    }
    if (runs) {
        bool simulated = false;
        for (const auto& c : setup.frame_configs)
            for (const auto& r : c.runs) simulated = simulated || r.origin == "simulated";
        s.measured.push_back({ui::fmt("%d run(s) across %d configuration(s)", runs, configs),
                              simulated ? ui::Origin::Simulated : ui::Origin::Measured});
        const auto cr = frame_config_runs(setup);
        if (cr.size() >= 2) {
            const auto rec = recommend_config(cr, FrameGoal::Consistency);
            s.recommended.push_back(rec.reliable ? "For consistency: \"" + cr[size_t(rec.best)].label + "\"" : rec.reason);
        } else {
            s.recommended.push_back("Add a candidate configuration to compare against your baseline.");
        }
    } else {
        s.needs.push_back("Record baseline runs with PresentMon (or import a PresentMon CSV).");
        s.recommended.push_back("No recommendation without measurements.");
    }
    return s;
}

ToolStatus motion_status(App& app) {
    ToolStatus s;
    auto& setup = app.setup();
    if (!setup.motion_sessions.empty()) {
        const auto& m = setup.motion_sessions.back();
        s.measured.push_back({ui::fmt("%zu motion session(s), last %s", setup.motion_sessions.size(),
                                      short_time(m.timestamp_utc).c_str()),
                              origin_of(m.origin)});
    } else {
        s.needs.push_back("Compare refresh rates or overdrive settings with the motion-clarity test.");
    }
    if (!setup.motion_selected.empty()) {
        s.recommended.push_back("Your setting: " + setup.motion_selected);
    } else if (!setup.motion_sessions.empty()) {
        // What the latest blinded identification test favours, if anything.
        const auto& m = setup.motion_sessions.back();
        const bool blinded = !m.configs.empty() &&
                             std::all_of(m.configs.begin(), m.configs.end(), [](const fit::MotionConfig& c) { return c.app_controlled; });
        std::string favoured;
        if (m.test == "identification" && blinded)
            for (size_t c = 1; c < m.configs.size(); ++c)
                if (fit::compare_motion(m.responses, 0, int(c)).verdict == "better") favoured = m.configs[c].label;
        if (!favoured.empty())
            s.recommended.push_back("The latest blinded test favours " + favoured + " over " + m.configs[0].label +
                                    ". Choose it on the Monitor tab to apply it.");
        else
            s.recommended.push_back("No setting stood out in the latest test. Keep your current one.");
    } else {
        s.recommended.push_back("No monitor setting selected yet.");
    }
    if (setup.display.overdrive.empty())
        s.needs.push_back("Enter your monitor's current overdrive setting (Monitor tab) so results can be tracked.");
    return s;
}

void tool_card(App& app, const char* id, const char* title, const char* what, const ToolStatus& st, Page page, float w, float h) {
    ui::begin_card(id, w, h);
    ui::card_title(title, what);
    ImGui::Separator();
    auto head = [](const char* h, unsigned c) {
        ui::push_font(fonts().semibold, theme::kSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(c));
        ImGui::TextUnformatted(h);
        ImGui::PopStyleColor();
        ui::pop_font();
    };
    head("MEASURED", theme::kAccent);
    if (st.measured.empty()) ui::faint("Nothing yet.");
    for (const auto& [line, origin] : st.measured) {
        ui::wrapped(line.c_str());
        if (origin == ui::Origin::Simulated) ui::origin_badge(origin);
    }
    ImGui::Dummy(ImVec2(0, 2));
    head("RECOMMENDED", theme::kGood);
    for (const auto& r : st.recommended) ui::wrapped(r.c_str(), theme::kText);
    ImGui::Dummy(ImVec2(0, 2));
    head("NEEDS TESTING", theme::kWarn);
    if (st.needs.empty()) ui::faint("Nothing pending.");
    for (size_t i = 0; i < std::min<size_t>(st.needs.size(), 4); ++i) ui::wrapped(("• " + st.needs[i]).c_str(), theme::kTextMuted);
    ImGui::Dummy(ImVec2(0, 4));
    if (ui::button_secondary(ui::fmt("Open the %s tab", title).c_str())) app.go(page);
    ui::end_card();
}

}  // namespace

// Everything FitSuite knows about this PC, with where each value came from (Nerd stats → System).
void overview_details(App& app) {
    auto& setup = app.setup();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = 14.0f;
    const float w = (avail - 2 * gap) / 3.0f;
    // Equal-height tool cards: measure the tallest content first (estimated from line counts).
    const ToolStatus ms = mouse_status(app), fs = frame_status(app), mo = motion_status(app);
    auto lines = [&](const ToolStatus& t) {
        float h = 0;
        const float lh = ImGui::GetTextLineHeightWithSpacing();
        auto wrapped_lines = [&](const std::string& s) { return std::max(1.0f, std::ceil(ImGui::CalcTextSize(s.c_str()).x / (w - 40))); };
        for (const auto& m : t.measured) h += wrapped_lines(m.first) * lh;
        if (t.measured.empty()) h += lh;
        for (const auto& r : t.recommended) h += wrapped_lines(r) * lh;
        for (size_t i = 0; i < std::min<size_t>(t.needs.size(), 4); ++i) h += wrapped_lines("• " + t.needs[i]) * lh;
        if (t.needs.empty()) h += lh;
        return h;
    };
    if (setup.app.show_extra_tools) {
        const float card_h = std::max({lines(ms), lines(fs), lines(mo)}) + 250.0f;
        tool_card(app, "##ov_mouse", "Mouse", "MouseFit: directional mouse calibration", ms, Page::MouseFit, w, card_h);
        ImGui::SameLine(0, gap);
        tool_card(app, "##ov_frame", "Frame rate", "FrameFit: frame pacing on your PC", fs, Page::FrameFit, w, card_h);
        ImGui::SameLine(0, gap);
        tool_card(app, "##ov_motion", "Monitor", "MotionFit: motion clarity on your monitor", mo, Page::MotionFit, w, card_h);
    } else {  // the extra tools are off (Settings): the mouse fix alone
        tool_card(app, "##ov_mouse", "Mouse", "MouseFit: directional mouse calibration", ms, Page::MouseFit, avail, 0.0f);
    }

    ui::section("Your setup");
    const float w2 = (avail - gap) / 2.0f;
    ui::begin_card("##ov_hw", w2, 0);
    ui::card_title("Hardware", "Detected through Windows where possible; values you typed are marked.");
    auto row = [](const char* k, const std::string& v, ui::Origin o) {
        ui::kv(k, v);
        ImGui::SameLine();
        ui::origin_badge(o);
    };
    const bool det_mouse = setup.mouse.source == "detected";
    row("Mouse", setup.mouse.name.empty() ? "Not selected" : setup.mouse.name + (setup.mouse.vid_pid.empty() ? "" : "  (" + setup.mouse.vid_pid + ")"),
        det_mouse ? ui::Origin::Detected : setup.mouse.source == "simulated" ? ui::Origin::Simulated : ui::Origin::NotAvailable);
    row("DPI", std::to_string(setup.mouse.dpi), ui::Origin::UserEntered);
    row("Report rate", setup.mouse.polling_hz > 0 ? ui::fmt("%.0f Hz", setup.mouse.polling_hz) : "Measured during calibration",
        setup.mouse.polling_hz <= 0 ? ui::Origin::NotAvailable : app.platform().dev_harness() ? ui::Origin::Simulated
                                                                                              : ui::Origin::Measured);
    const bool det_pc = setup.pc.source == "detected";
    row("CPU", setup.pc.cpu, det_pc ? ui::Origin::Detected : ui::Origin::NotAvailable);
    row("GPU", setup.pc.gpus.empty() ? "" : setup.pc.gpus.front().name + "  ·  driver " + setup.pc.gpus.front().driver,
        det_pc ? ui::Origin::Detected : ui::Origin::NotAvailable);
    row("Memory", setup.pc.ram_gb > 0 ? ui::fmt("%.0f GB", setup.pc.ram_gb) : "", det_pc ? ui::Origin::Detected : ui::Origin::NotAvailable);
    row("Windows", setup.pc.os, det_pc ? ui::Origin::Detected : ui::Origin::NotAvailable);
    const ui::Origin disp_origin = setup.display.source == "detected" ? ui::Origin::Detected
                                   : setup.display.source == "simulated" ? ui::Origin::Simulated : ui::Origin::NotAvailable;
    row("Monitor", setup.display.name, disp_origin);
    row("Display mode",
        setup.display.width > 0 ? ui::fmt("%d × %d at %.2f Hz", setup.display.width, setup.display.height, setup.display.refresh_hz) : "",
        disp_origin);
    row("Overdrive", setup.display.overdrive.empty() ? "Not entered" : setup.display.overdrive,
        setup.display.overdrive.empty() ? ui::Origin::NotAvailable : ui::Origin::UserEntered);
    ImGui::Dummy(ImVec2(0, 4));
    if (ui::button_secondary("Detect hardware again")) {
        app.refresh_hardware();
        app.save();
        app.toast(ui::Tone::Info, "Hardware information refreshed.");
    }
    ui::end_card();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ui::begin_card("##ov_game", w2, 0);
    const std::string game_title = "Game: " + main_game_name(setup);
    ui::card_title(game_title.c_str(), "Settings you entered. FitSuite never changes game settings.");
    if (fit::game_yaw(fit::main_game_exe(setup.game.id)) > 0.0) ui::kv("Sensitivity", ui::num(setup.game.sensitivity, 3));
    ui::kv("Scoped multiplier", ui::num(setup.game.scoped_multiplier, 2));
    ui::kv("FPS limit", setup.game.fps_cap > 0 ? std::to_string(setup.game.fps_cap) : "Unlimited / not entered");
    const double cm360 = main_game_cm360(setup);
    ui::kv("Distance per 360°", cm360 > 0 ? ui::fmt("%.1f cm", cm360) : "");
    ImGui::SameLine();
    ui::origin_badge(ui::Origin::Estimate);
    ui::end_card();
    ImGui::Dummy(ImVec2(0, 4));
    ui::begin_card("##ov_base", w2, 0);
    ui::card_title("Baseline", "A snapshot of your current settings, so you can always return to them.");
    if (setup.baseline_captured_utc.empty()) {
        ui::muted("Not recorded yet. Record it before trying candidate settings.");
    } else {
        ui::kv("Recorded", short_time(setup.baseline_captured_utc));
        ui::kv("Items", std::to_string(setup.baseline.size()) + " settings");
    }
    if (ui::button_primary(setup.baseline_captured_utc.empty() ? "Record baseline now" : "Record baseline again"))
        app.capture_baseline();
    ImGui::SameLine();
    if (ui::button_secondary("Restore…")) app.go(Page::Profiles);
    ui::end_card();
    ImGui::EndGroup();

    ui::section("Recent results");
    ui::begin_card("##ov_recent", 0, 0);
    if (setup.results.empty()) ui::faint("No results yet. Start on the Mouse or Frame rate tab.");
    if (ImGui::BeginTable("##recent", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("When", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Result");
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 180);
        int shown = 0;
        for (auto it = setup.results.rbegin(); it != setup.results.rend() && shown < 8; ++it, ++shown) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ui::muted("%s", short_time(it->timestamp_utc).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->tool == "mousefit" ? "MouseFit" : it->tool == "framefit" ? "FrameFit" : "MotionFit");
            ImGui::TableNextColumn();
            ui::wrapped(it->summary.c_str());
            ImGui::TableNextColumn();
            const Staleness st = check_staleness(it->conditions, conditions_for(it->tool, setup));
            if (it->origin == "simulated") ui::origin_badge(ui::Origin::Simulated);
            else if (st.outdated) {
                ui::pill("POTENTIALLY OUTDATED", theme::kWarn);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.reasons.front().c_str());
            } else ui::origin_badge(ui::Origin::Measured);
        }
        ImGui::EndTable();
    }
    ui::end_card();
}

// ---- Home (simple) -----------------------------------------------------------------------------------
// ---- Nerd stats ----------------------------------------------------------------------------------
void page_nerd(App& app) {
    ui::page_header("Nerd stats", "Every number, chart and setting behind the simple views: raw measurements, model fits, "
                                  "confidence intervals and all the controls.");
    // Frame rate and Monitor appear with the extra tools (Settings).
    const bool extras = app.setup().app.show_extra_tools;
    if (!extras && (app.nerd_tab == 1 || app.nerd_tab == 2)) app.nerd_tab = 0;
    std::vector<const char*> labels = {"Mouse"};
    std::vector<int> ids = {0};
    if (extras) {
        labels.insert(labels.end(), {"Frame rate", "Monitor"});
        ids.insert(ids.end(), {1, 2});
    }
    labels.push_back("System");
    ids.push_back(3);
    int current = 0;
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == app.nerd_tab) current = int(i);
    const int clicked = ui::tabs(labels.data(), int(labels.size()), current);
    if (clicked >= 0) app.nerd_tab = ids[size_t(clicked)];
    ImGui::Dummy(ImVec2(0, 6));
    switch (app.nerd_tab) {
        case 0: mousefit_details(app); break;
        case 1: framefit_details(app); break;
        case 2: motionfit_details(app); break;
        default: overview_details(app); break;
    }
}

}  // namespace fitapp
