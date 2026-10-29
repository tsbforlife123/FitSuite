#include <algorithm>
#include <cctype>
#include <cmath>

#include "app.hpp"
#include "fit/mouse_db.hpp"
#include "fit/rawaccel_export.hpp"
#include "fit/store.hpp"
#include "fit/timeutil.hpp"
#include "imgui.h"
#include "implot.h"

namespace fitapp {

using namespace fit;
using theme::fonts;

namespace {

const char* kDirNames[8] = {"Right", "Up-right", "Up", "Up-left", "Left", "Down-left", "Down", "Down-right"};
const char* kDirShort[8] = {"R", "UR", "U", "UL", "L", "DL", "D", "DR"};

ImPlotSpec line_spec(unsigned c, float w = 2.0f) {
    ImPlotSpec s;
    s.LineColor = theme::v4(c);
    s.LineWeight = w;
    return s;
}

ImPlotSpec bar_spec(unsigned c) {
    ImPlotSpec s;
    s.FillColor = theme::v4(c, 0.80f);
    s.LineColor = theme::v4(c);
    s.LineWeight = 0.0f;
    return s;
}

ImPlotSpec err_spec() {
    ImPlotSpec s;
    s.LineColor = theme::v4(theme::kTextMuted);
    s.LineWeight = 1.5f;
    s.Size = 6.0f;
    s.Flags = ImPlotItemFlags_NoLegend;
    return s;
}

RawAccelProfile scale_rawaccel(const RawAccelProfile& p, double s) {
    RawAccelProfile r;
    r.rotation_deg = p.rotation_deg * s;
    r.yx_ratio = std::pow(p.yx_ratio, s);
    r.lr_ratio = std::pow(p.lr_ratio, s);
    r.ud_ratio = std::pow(p.ud_ratio, s);
    double g = 0.0;
    for (int i = 0; i < 360; ++i) g += r.gain((i + 0.5) * kDegToRad);
    r.sens = 360.0 / g;
    return r;
}

// Polar summary: one spoke per direction. Bars outside the reference ring are overshoot, inside
// are undershoot; the dashed ring is the player's own average for this session.
void direction_wheel(const std::vector<DirectionSummary>& dirs, float size) {
    using namespace theme;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    // Radii, from the outside in: labels, angle dots, then the band the distance bars live in.
    const float half = size * 0.5f;
    const float r_dot = half - 58.0f;  // labels sit just outside the dots (see below)
    const float r_max = r_dot - 10.0f, r_min = 16.0f;
    const float R = 0.5f * (r_min + r_max);  // zero error
    double extent = 5.0;                     // % shown at the band edge (at least ±5 %)
    double mean = 0.0;
    int n = 0;
    for (const auto& d : dirs)
        if (d.n) {
            mean += d.mean_radial_error_pct;
            ++n;
            extent = std::max(extent, std::fabs(d.mean_radial_error_pct) + d.sd_radial_error_pct);
        }
    mean = n ? mean / n : 0.0;
    const float scale = float((r_max - R) / extent);  // px per percent
    auto at = [&](float r, float a) { return ImVec2(c.x + r * std::cos(a), c.y + r * std::sin(a)); };
    dl->AddCircle(c, R, rgb(kBorderHi), 64, 1.0f);
    const float rm = R + float(mean) * scale;
    for (int i = 0; i < 64; i += 2)
        dl->AddLine(at(rm, i * 6.2831853f / 64), at(rm, (i + 1) * 6.2831853f / 64), rgb(kTextFaint), 1.0f);
    ui::push_font(fonts().medium, kSmall);
    for (size_t i = 0; i < dirs.size() && i < 8; ++i) {
        const auto& d = dirs[i];
        const float a = -float(d.dir_deg * kDegToRad);  // screen y is down
        dl->AddLine(at(r_min - 6, a), at(r_max + 4, a), rgb(kSpoke), 1.0f);
        if (d.n) {
            const float r1 = R + float(d.mean_radial_error_pct) * scale;
            const unsigned col = d.mean_radial_error_pct >= 0 ? kWarn : kInfo;
            dl->AddLine(at(R, a), at(r1, a), rgb(col), 6.0f);
            // Repeatability: ±1 SD whisker.
            const float lo = R + float(d.mean_radial_error_pct - d.sd_radial_error_pct) * scale;
            const float hi = R + float(d.mean_radial_error_pct + d.sd_radial_error_pct) * scale;
            dl->AddLine(at(std::max(lo, r_min), a), at(std::min(hi, r_max), a), rgb(kTextMuted, 0.6f), 1.0f);
            // Angle error: a dot rotated by the mean angular error (exaggerated ×4).
            dl->AddCircleFilled(at(r_dot, a - float(d.mean_angle_error_deg * 4.0 * kDegToRad)), 3.0f, rgb(kAccent));
        }
        const std::string label = ui::fmt("%s %s", kDirShort[i], d.n ? ui::signed_num(d.mean_radial_error_pct, 1).c_str() : "—");
        const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        // Push the label out until its inner edge clears the dot ring, whatever the direction.
        const float extent = 0.5f * (std::fabs(std::cos(a)) * ts.x + std::fabs(std::sin(a)) * ts.y);
        const ImVec2 lp = at(r_dot + 12.0f + extent, a);
        dl->AddText(ImVec2(lp.x - ts.x * 0.5f, lp.y - ts.y * 0.5f), rgb(kTextMuted), label.c_str());
    }
    ui::pop_font();
}

// "down +6.3% · up −1.6% · left −3.2% · right −1.6%": how much each direction's sensitivity changes.
std::string direction_summary(const Transform& t) {
    auto g = [&](double deg) {
        const double th = deg * kDegToRad;
        const double v = t.kind == TransformKind::RawAccel ? t.raw_accel.gain(th)
                         : t.kind == TransformKind::Directional ? t.directional.gain(th) : 1.0;
        return (v - 1.0) * 100.0;
    };
    return ui::fmt("down %+.1f%% · up %+.1f%% · left %+.1f%% · right %+.1f%%", g(-90), g(90), g(180), g(0));
}

void correction_plots(const char* id, const Transform& t, const Transform* other, float height) {
    static double xs[361];
    static bool init = false;
    if (!init) {
        for (int i = 0; i <= 360; ++i) xs[i] = i;
        init = true;
    }
    double g[361], a[361], g2[361], a2[361];
    auto eval = [](const Transform& tr, double deg, double& gain_pct, double& angle_deg) {
        const double th = deg * kDegToRad;
        if (tr.kind == TransformKind::Directional) {
            gain_pct = (tr.directional.gain(th) - 1.0) * 100.0;
            angle_deg = tr.directional.angle(th) * kRadToDeg;
        } else if (tr.kind == TransformKind::RawAccel) {
            gain_pct = (tr.raw_accel.gain(th) - 1.0) * 100.0;
            angle_deg = tr.raw_accel.angle(th) * kRadToDeg;
        } else {
            gain_pct = angle_deg = 0.0;
        }
    };
    for (int i = 0; i <= 360; ++i) {
        eval(t, xs[i], g[i], a[i]);
        if (other) eval(*other, xs[i], g2[i], a2[i]);
    }
    static const double ticks[9] = {0, 45, 90, 135, 180, 225, 270, 315, 360};
    static const char* tick_labels[9] = {"R", "UR", "U", "UL", "L", "DL", "D", "DR", "R"};
    double gmax = 0.0, amax = 0.0;
    for (int i = 0; i <= 360; ++i) {
        gmax = std::max({gmax, std::fabs(g[i]), other ? std::fabs(g2[i]) : 0.0});
        amax = std::max({amax, std::fabs(a[i]), other ? std::fabs(a2[i]) : 0.0});
    }
    const double glim = std::max(10.0, std::ceil(gmax * 1.25)), alim = std::max(4.0, std::ceil(amax * 1.25));
    const float w = (ImGui::GetContentRegionAvail().x - 10) * 0.5f;
    for (int k = 0; k < 2; ++k) {
        if (k) ImGui::SameLine(0, 10);
        const std::string pid = ui::fmt("%s##%s%d", k == 0 ? "Sensitivity change" : "Direction change", id, k);
        if (ImPlot::BeginPlot(pid.c_str(), ImVec2(w, height), ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
            ImPlot::SetupAxes("Movement direction", k == 0 ? "Change (%)" : "Rotation (°)", ImPlotAxisFlags_Lock, 0);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, 360, ImPlotCond_Always);
            ImPlot::SetupAxisTicks(ImAxis_X1, ticks, 9, tick_labels);
            ImPlot::SetupAxisLimits(ImAxis_Y1, k == 0 ? -glim : -alim, k == 0 ? glim : alim, ImPlotCond_Always);
            ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Outside | ImPlotLegendFlags_Horizontal);
            const double zero[2] = {0, 0}, zx[2] = {0, 360};
            ImPlot::PlotLine("##zero", zx, zero, 2, line_spec(theme::kBorderHi, 1.0f));
            ImPlot::PlotLine(t.kind == TransformKind::RawAccel ? "Raw Accel-compatible" : "Smooth directional", xs, k == 0 ? g : a,
                             361, line_spec(t.kind == TransformKind::RawAccel ? theme::kWarn : theme::kAccent, 2.5f));
            if (other)
                ImPlot::PlotLine(other->kind == TransformKind::RawAccel ? "Raw Accel-compatible" : "Smooth directional", xs,
                                 k == 0 ? g2 : a2, 361,
                                 line_spec(other->kind == TransformKind::RawAccel ? theme::kWarn : theme::kAccent, 1.5f));
            ImPlot::EndPlot();
        }
    }
}

// ---- tabs ---------------------------------------------------------------------------------------

void tab_setup(App& app) {
    auto& setup = app.setup();
    auto& ui_ = app.mouse;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float w = (avail - 14) * 0.5f;

    ui::begin_card("##mf_mouse", w, 0);
    ui::card_title("Mouse", "Raw input is read per device. Software-generated input is ignored.");
    auto devices = app.platform().mouse().devices();
    std::string current = "Any mouse (lock to the first one that moves)";
    for (const auto& d : devices)
        if (d.id == setup.mouse.device_id) current = d.name + (d.vid_pid.empty() ? "" : "  (" + d.vid_pid + ")");
    (void)current;
    const float row_h = ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::BeginListBox("##mouse_dev", ImVec2(-1, row_h * float(std::min<size_t>(devices.size() + 1, 5)) + 6))) {
        if (ImGui::Selectable("Any mouse (lock to the first one that moves)", setup.mouse.device_id.empty())) {
            setup.mouse.device_id.clear();
            app.save();
        }
        for (const auto& d : devices) {
            std::string label = (d.name.empty() ? std::string("Unnamed mouse") : d.name) +
                                (d.vid_pid.empty() ? "" : "  (" + d.vid_pid + ")") + (d.absolute ? "  — absolute, not usable" : "");
            ImGui::BeginDisabled(d.absolute);
            if (ImGui::Selectable((label + "##" + d.id).c_str(), d.id == setup.mouse.device_id)) {
                setup.mouse.device_id = d.id;
                setup.mouse.name = d.name;
                setup.mouse.vid_pid = d.vid_pid;
                setup.mouse.source = "detected";
                app.save();
            }
            ImGui::EndDisabled();
        }
        ImGui::EndListBox();
    }
    if (!ui_.identifying) {
        if (ui::button_secondary("Identify by moving it")) {
            std::string err;
            if (app.platform().mouse().start(&err)) ui_.identifying = true;
            else app.toast(ui::Tone::Bad, "Raw input could not be started: " + err);
        }
        ui::help_marker("Starts raw input briefly. Move the mouse you play with; FitSuite selects it and stops listening.");
    } else {
        ui::callout(ui::Tone::Info, "Move the mouse you play with…", "Listening for raw input from this window only.");
        std::vector<RawEvent> ev;
        app.platform().mouse().drain(ev);
        for (const auto& e : ev) {
            if (e.injected || e.absolute || (e.dx == 0 && e.dy == 0) || e.device_id.empty()) continue;
            if (std::abs(e.dx) + std::abs(e.dy) < 3) continue;
            app.platform().mouse().stop();
            ui_.identifying = false;
            setup.mouse.device_id = e.device_id;
            if (auto d = app.platform().mouse().device(e.device_id)) {
                setup.mouse.name = d->name;
                setup.mouse.vid_pid = d->vid_pid;
                setup.mouse.source = "detected";
            }
            app.save();
            app.toast(ui::Tone::Good, "Selected " + (setup.mouse.name.empty() ? std::string("the mouse") : setup.mouse.name) + ".");
            break;
        }
        if (ui_.identifying && (ui::button_secondary("Cancel") || !app.platform().window_focused())) {
            app.platform().mouse().stop();
            ui_.identifying = false;
        }
    }
    ImGui::Dummy(ImVec2(0, 4));
    int dpi = setup.mouse.dpi;
    ImGui::SetNextItemWidth(160);
    if (ImGui::InputInt("DPI (CPI)", &dpi, 50, 400)) {
        setup.mouse.dpi = std::clamp(dpi, 50, 64000);
        app.save();
    }
    ui::help_marker("Mice do not report their DPI to Windows. Use the value set in your mouse software.");
    ui::kv("Report rate", setup.mouse.polling_hz > 0 ? ui::fmt("%.0f Hz (measured)", setup.mouse.polling_hz) : "Measured during calibration");
    if (app.platform().rawaccel_driver_present())
        ui::callout(ui::Tone::Warn, "Raw Accel driver detected",
                    "FitSuite measures your mouse after Raw Accel's processing. Calibrate with the Raw Accel settings you "
                    "normally play with; exports are combined with them.");
    ui::end_card();

    ImGui::SameLine(0, 14);
    ui::begin_card("##mf_game", w, 0);
    const double yaw = game_yaw(main_game_exe(setup.game.id));
    const std::string game_note =
        yaw > 0.0 ? ui::fmt("FitSuite's test scene turns the view exactly like %s: %.4g° per count × sensitivity.",
                            main_game_name(setup).c_str(), yaw)
                  : ui::fmt("FitSuite's test scene turns the view by the distance you entered for a full turn in %s.",
                            main_game_name(setup).c_str());
    ui::card_title(main_game_sens_label(setup).c_str(), game_note.c_str());
    main_game_sens_input(app, 160);
    float scoped = float(setup.game.scoped_multiplier);
    ImGui::SetNextItemWidth(160);
    if (ImGui::InputFloat("Scoped multiplier", &scoped, 0.05f, 0.1f, "%.2f")) {
        setup.game.scoped_multiplier = std::clamp(double(scoped), 0.01, 10.0);
        app.save();
    }
    const GameMapping m = main_game_mapping(setup);
    const double cm360 = 360.0 / m.deg_per_count() / std::max(setup.mouse.dpi, 1) * 2.54;
    ImGui::Dummy(ImVec2(0, 4));
    ui::metric("per count", ui::num(m.deg_per_count(), 4), "°", ui::Origin::Estimate);
    ImGui::SameLine(0, 34);
    ui::metric("for a full turn", ui::num(cm360, 1), "cm", ui::Origin::Estimate);
    ImGui::SameLine(0, 34);
    ui::metric("eDPI", ui::num(setup.game.sensitivity * setup.mouse.dpi, 0), "", ui::Origin::Estimate);
    ImGui::Dummy(ImVec2(0, 2));
    ui::faint("Calculated from the values you entered. Scoped aim is not calibrated in this version.");
    ui::end_card();

    ui::section("What MouseFit can and cannot do");
    ui::begin_card("##mf_caps", 0, 0);
    if (ImGui::BeginTable("##caps", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Capability", ImGuiTableColumnFlags_WidthFixed, 260);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 170);
        ImGui::TableSetupColumn("Details");
        auto row = [](const char* a, const char* st, unsigned c, const char* d) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(a);
            ImGui::TableNextColumn();
            ui::pill(st, c);
            ImGui::TableNextColumn();
            ui::wrapped(d, theme::kTextMuted);
        };
        row("Read your mouse", "SUPPORTED", theme::kAccent,
            "Windows raw input, per device, only while a test runs and FitSuite has focus.");
        row("Create and test a directional profile", "SUPPORTED", theme::kAccent,
            "Calibration, fitting and blinded validation all run inside FitSuite.");
        row("Apply it in VALORANT", "PARTIAL", theme::kWarn,
            "The Raw Accel-compatible profile (rotation plus separate up, down, left and right sensitivity) can be applied "
            "through Raw Accel, which then adjusts every movement by its direction. FitSuite can switch it on only while "
            "your chosen games are in front. FitSuite has not verified Raw Accel in VALORANT. The smooth directional "
            "profile needs a signed driver that does not exist yet.");
        ImGui::EndTable();
    }
    ui::end_card();
    ImGui::Dummy(ImVec2(0, 6));
    if (ui::button_primary("Continue to calibration")) ui_.tab = 1;
}

void tab_calibrate(App& app) {
    auto& ui_ = app.mouse;
    auto& setup = app.setup();
    ui::begin_card("##mf_cal", 0, 0);
    ui::card_title("Calibration",
                   "Rounds like an aim trainer: flicks (8 directions × 4 distances, out from the centre and back), "
                   "micro-adjustments, tracking and target switching. A new target appears as soon as the last one is hit "
                   "or missed, and every target has a timer. The first flick toward each target is what MouseFit measures.");
    ImGui::SetNextItemWidth(150);
    if (ImGui::InputInt("Flick rounds", &ui_.calib_blocks, 1, 1)) ui_.calib_blocks = std::clamp(ui_.calib_blocks, 1, 4);
    ImGui::SameLine(0, 28);
    ImGui::SetNextItemWidth(150);
    if (ImGui::InputFloat("Target radius (°)", &ui_.radius_deg, 0.1f, 0.1f, "%.1f"))
        ui_.radius_deg = std::clamp(ui_.radius_deg, 0.8f, 2.0f);
    ImGui::SameLine();
    ui::help_marker("Flick rounds: 1 to 4 (64 targets each). More rounds give a more precise measurement.\nTarget radius "
                    "(flick rounds): 0.8° to 2.0°. Larger targets make flicks less careful; 1.2° is close to a head at "
                    "medium range.");
    const int flicks = ui_.calib_blocks * 64 + 32 + 36;
    ui::muted("%d targets + 40 s of tracking · about %d minutes · flick distances 7°, 13°, 20° and 26°", flicks,
              int(std::lround((ui_.calib_blocks * 55 + 25 + 40 + 40 + 25) / 60.0)));
    const bool ready = main_game_ready(setup);
    if (!ready) ui::callout(ui::Tone::Warn, "Enter your DPI and sensitivity first", nullptr);
    ImGui::BeginDisabled(!ready);
    if (ui::button_primary("Start calibration")) app.start_calibration();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ui::faint("Full screen · Esc stops at once · leaving the window pauses and releases the mouse");
    ui::end_card();

    if (!ui_.calibration) {
        ui::section("Results");
        ui::faint("No calibration yet.");
        return;
    }
    const Session& s = *ui_.calibration;
    ui::section("Latest calibration");
    ImGui::BeginGroup();
    ui::muted("%s · %s · %s", short_time(s.finished_utc).c_str(), s.conditions.mouse_name.empty() ? "mouse" : s.conditions.mouse_name.c_str(),
              ui::fmt("%d DPI, sensitivity %.3f", s.conditions.dpi, s.conditions.mapping.sensitivity).c_str());
    ImGui::SameLine();
    ui::origin_badge(origin_of(s.conditions.data_origin));
    if (!s.complete) {
        ImGui::SameLine();
        ui::pill("INCOMPLETE", theme::kWarn);
    }
    Conditions rec;
    rec["mouse.device"] = !s.conditions.mouse_vid_pid.empty() ? s.conditions.mouse_vid_pid : s.conditions.mouse_name;
    rec["mouse.dpi"] = std::to_string(s.conditions.dpi);
    rec["game.sensitivity"] = ui::fmt("%.6g", s.conditions.mapping.sensitivity);
    if (rec["mouse.device"].empty()) rec.erase("mouse.device");
    staleness_line(app, rec, "mousefit");
    ImGui::EndGroup();

    const OverallSummary o = summarize_overall(ui_.calib_metrics);
    const auto dirs = summarize_by_direction(ui_.calib_metrics, 8);
    ui::begin_card("##mf_tiles", 0, 0);
    const float cw = (ImGui::GetContentRegionAvail().x) / 6.0f;
    auto tile = [&](int i, const char* label, const std::string& v, const char* unit) {
        if (i) ImGui::SameLine(cw * i + 16);
        ui::metric(label, v, unit, ui::Origin::None);
    };
    tile(0, "flicks used", ui::fmt("%d", o.n_valid), "");
    tile(1, "median reaction", ui::num(o.median_reaction_ms, 0), "ms");
    tile(2, "median correction", ui::num(o.median_correction_ms, 0), "ms");
    tile(3, "first-click hits", ui::num(o.first_click_hit_rate * 100, 0), "%");
    tile(4, "mean |distance error|", ui::num(o.mean_abs_radial_error_pct, 1), "%");
    tile(5, "mean |angle error|", ui::num(o.mean_abs_angle_error_deg, 1), "°");
    ImGui::Dummy(ImVec2(0, 6));
    ui::muted("First flicks: %d undershoot · %d on target · %d overshoot. %d trials excluded (%d timeouts counted as misses).",
              o.n_under, o.n_on, o.n_over, o.n_invalid, o.n_timeouts);
    const auto& c = s.conditions;
    if (c.render_fps_median > 0) {
        const bool unstable = c.render_frame_p99_ms > 3.0 * (1000.0 / c.render_fps_median) || c.render_hitches > 20;
        ui::muted("Test scene: %.0f fps median, 99th percentile frame %.2f ms, %d hitches · display %.0f Hz · report rate %.0f Hz",
                  c.render_fps_median, c.render_frame_p99_ms, c.render_hitches, c.display_refresh_hz, c.polling_hz_measured);
        if (unstable)
            ui::callout(ui::Tone::Warn, "Rendering was uneven during this calibration",
                        "Frame pacing problems can look like aiming problems. Close background apps and recalibrate before "
                        "trusting small directional differences.");
    }
    ui::end_card();

    const float avail = ImGui::GetContentRegionAvail().x;
    ui::begin_card("##mf_wheel", 380, 0);
    ui::card_title("Direction wheel", "Bars: first-flick distance error by direction (outside = overshoot, inside = "
                                      "undershoot); dashed ring: your average; dots: angle error (×4).");
    direction_wheel(dirs, 340);
    ui::end_card();
    ImGui::SameLine(0, 14);
    ui::begin_card("##mf_bars", avail - 380 - 14, 0);
    ui::card_title("By direction", "Mean first-flick error with ±1 standard deviation (repeatability).");
    double xs[8], re[8], rsd[8], ae[8], asd[8];
    for (int i = 0; i < 8; ++i) {
        xs[i] = i;
        re[i] = dirs[size_t(i)].mean_radial_error_pct;
        rsd[i] = dirs[size_t(i)].sd_radial_error_pct;
        ae[i] = dirs[size_t(i)].mean_angle_error_deg;
        asd[i] = dirs[size_t(i)].sd_angle_error_deg;
    }
    static const double ticks[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    const float pw = ImGui::GetContentRegionAvail().x;
    if (ImPlot::BeginPlot("Distance error (+ overshoot, − undershoot)##mfr", ImVec2(pw, 170),
                          ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes(nullptr, "%", ImPlotAxisFlags_Lock, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisTicks(ImAxis_X1, ticks, 8, kDirShort);
        ImPlot::SetupAxisLimits(ImAxis_X1, -0.6, 7.6, ImPlotCond_Always);
        ImPlot::PlotBars("Distance error", xs, re, 8, 0.55, bar_spec(theme::kInfo));
        ImPlot::PlotErrorBars("##sd", xs, re, rsd, 8, err_spec());
        ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("Angle error (+ counter-clockwise)##mfa", ImVec2(pw, 150),
                          ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes(nullptr, "°", ImPlotAxisFlags_Lock, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisTicks(ImAxis_X1, ticks, 8, kDirShort);
        ImPlot::SetupAxisLimits(ImAxis_X1, -0.6, 7.6, ImPlotCond_Always);
        ImPlot::PlotBars("Angle error", xs, ae, 8, 0.55, bar_spec(theme::kAccent));
        ImPlot::PlotErrorBars("##asd", xs, ae, asd, 8, err_spec());
        ImPlot::EndPlot();
    }
    ui::end_card();

    ui::begin_card("##mf_table", 0, 0);
    if (ImGui::BeginTable("##dirs", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchSame)) {
        for (const char* h : {"Direction", "Flicks", "Distance error", "Repeatability (SD)", "Angle error", "Curve", "Straightness",
                              "Median time to hit", "First-click hits"})
            ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (int i = 0; i < 8; ++i) {
            const auto& d = dirs[size_t(i)];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kDirNames[i]);
            ui::push_font(fonts().mono, theme::kMono);
            ImGui::TableNextColumn();
            ImGui::Text("%d", d.n);
            ImGui::TableNextColumn();
            ImGui::Text("%+.1f %%", d.mean_radial_error_pct);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f %% / %.1f°", d.sd_radial_error_pct, d.sd_angle_error_deg);
            ImGui::TableNextColumn();
            ImGui::Text("%+.2f°", d.mean_angle_error_deg);
            ImGui::TableNextColumn();  // + bows left of a straight line, − right
            if (d.n) ImGui::Text("%.1f %% %s", std::fabs(d.mean_curve_pct), d.mean_curve_pct >= 0 ? "left" : "right");
            ImGui::TableNextColumn();
            if (d.n) ImGui::Text("%.0f %%", d.mean_straightness_pct);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f ms", d.median_acquisition_ms);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f %%", d.first_click_hit_rate * 100);
            ui::pop_font();
        }
        ImGui::EndTable();
    }
    ui::end_card();
    if (ui_.fit && ui_.fit->ok && ui::button_primary("Review candidate corrections")) ui_.tab = 2;
}

void profile_card(App& app, MouseProfileRecord& m, const FitResult* r) {
    auto& ui_ = app.mouse;
    ImGui::PushID(m.id.c_str());
    ui::begin_card("##prof", 0, 0);
    ui::push_font(fonts().semibold, theme::kH3);
    ImGui::TextUnformatted(m.label.c_str());
    ui::pop_font();
    ImGui::SameLine();
    if (m.transform.kind == TransformKind::RawAccel) ui::pill("RAW ACCEL-COMPATIBLE", theme::kWarn);
    else ui::pill("IN-APP ONLY", theme::kInfo);
    ImGui::SameLine();
    if (m.validation) {
        const bool good = m.validation->verdict == "improved";
        ui::pill(good ? "VALIDATED: BETTER" : m.validation->verdict == "worse" ? "VALIDATED: WORSE" : "NOT CONFIRMED",
                 good ? theme::kGood : theme::kTextMuted, good);
    } else {
        ui::pill("CANDIDATE · NOT VALIDATED", theme::kTextMuted);
    }
    if (m.origin == "simulated") {
        ImGui::SameLine();
        ui::origin_badge(ui::Origin::Simulated);
    }
    staleness_line(app, m.conditions, "mousefit");
    ImGui::TextUnformatted(("Sensitivity per direction: " + direction_summary(m.transform)).c_str());
    ImGui::SameLine();
    ui::help_marker("How much more (+) or less (−) sensitive each direction becomes while you move that way. Your "
                    "average sensitivity stays the same: directions are balanced against each other.");
    if (m.transform.kind == TransformKind::RawAccel) {
        const auto& p = m.transform.raw_accel;
        ui::muted("Raw Accel: rotation %+.2f° · Y/X ratio %.4f · L/R ratio %.4f · U/D ratio %.4f · sensitivity %.4f", p.rotation_deg,
                  p.yx_ratio, p.lr_ratio, p.ud_ratio, p.sens);
    } else {
        ui::muted("Largest sensitivity change %.1f%% · largest direction change %.2f° · strength %.0f%%",
                  m.transform.directional.max_gain_deviation() * 100, m.transform.directional.max_angle_deg(),
                  m.transform.directional.strength * 100);
    }
    Transform other;
    const Transform* other_ptr = nullptr;
    if (r) {
        other.kind = m.transform.kind == TransformKind::RawAccel ? TransformKind::Directional : TransformKind::RawAccel;
        other.directional = r->directional;
        other.raw_accel = r->raw_accel;
        if (!other.is_identity()) other_ptr = &other;
    }
    correction_plots(m.id.c_str(), m.transform, other_ptr, 190);
    if (r) {
        const bool ra = m.transform.kind == TransformKind::RawAccel;
        ui::metric("largest directional distance bias: before → after", ui::fmt("%.1f → %.1f", r->max_radial_bias_before_pct,
                   ra ? r->max_radial_bias_after_ra_pct : r->max_radial_bias_after_dir_pct), "%", ui::Origin::Estimate);
        ImGui::SameLine(0, 40);
        ui::metric("largest directional angle bias: before → after", ui::fmt("%.1f → %.1f", r->max_angle_bias_before_deg,
                   ra ? r->max_angle_bias_after_ra_deg : r->max_angle_bias_after_dir_deg), "°", ui::Origin::Estimate);
        ui::faint("Estimate from the calibration flicks themselves (assumes your hand moves the same way). Only the blinded "
                  "comparison shows whether it actually helps.");
        if (ra && r->directional_recommended)
            ui::faint("Raw Accel can follow the smooth profile to within %.1f%% sensitivity and %.1f° direction.",
                      r->raw_accel_gain_mismatch * 100, r->raw_accel_angle_mismatch_deg);
    }
    if (m.transform.kind == TransformKind::Directional) {
        float st = float(m.transform.directional.strength * 100.0);
        ImGui::SetNextItemWidth(260);
        if (ImGui::SliderFloat("Strength", &st, 0.0f, 100.0f, "%.0f%%")) {
            m.transform.directional.strength = std::clamp(double(st) / 100.0, 0.0, 1.0);
            app.save();
        }
        if (m.validation && std::fabs(m.validation->strength - m.transform.directional.strength) > 1e-6) {
            ImGui::SameLine();
            ui::pill("STRENGTH CHANGED — VALIDATE AGAIN", theme::kWarn);
        }
    }
    ImGui::Dummy(ImVec2(0, 4));
    if (ui::button_primary("Validate (blinded)")) {
        ui_.selected_profile = m.id;
        ui_.tab = 3;
    }
    ImGui::SameLine();
    if (ui::button_secondary("Make a 50% version")) {
        MouseProfileRecord copy = m;
        copy.id = make_id("mp");
        copy.label = m.label + " (50%)";
        copy.created_utc = utc_now_iso();
        copy.validation.reset();
        copy.status = "candidate";
        if (copy.transform.kind == TransformKind::RawAccel) copy.transform.raw_accel = scale_rawaccel(m.transform.raw_accel, 0.5);
        else copy.transform.directional.strength = m.transform.directional.strength * 0.5;
        app.setup().mouse_profiles.push_back(copy);
        app.save();
        app.toast(ui::Tone::Info, "Created a weaker candidate. Validate it before using it.");
    }
    ImGui::SameLine();
    if (ui::button_danger("Discard")) {
        m.status = "discarded";
        if (app.setup().active_mouse_profile == m.id) app.setup().active_mouse_profile.clear();
        app.save();
    }
    ui::end_card();
    ImGui::PopID();
}

void tab_candidates(App& app) {
    auto& ui_ = app.mouse;
    if (!ui_.fit) {
        ui::faint("Run a calibration first.");
        return;
    }
    const FitResult& r = *ui_.fit;
    ui::callout(r.ok && (r.directional_recommended || r.raw_accel_recommended) ? ui::Tone::Info : ui::Tone::Neutral,
                r.ok ? "Fit result" : "Not enough data", r.message.c_str());
    for (const auto& pr : r.data_problems) ui::muted("• %s", pr.c_str());
    if (r.ok) {
        ImGui::Dummy(ImVec2(0, 2));
        ImGui::TextUnformatted("Statistical details");
        ui::push_font(fonts().mono, theme::kMono);
        ImGui::Text("flicks used %d, excluded as gross errors %d, invalid %d", r.n_used, r.n_excluded_gross, r.n_invalid);
        ImGui::Text("gain anisotropy:    Wald-F p = %.4f  (%s)", r.gain_test.p_value, r.gain_test.significant ? "kept" : "not significant");
        ImGui::Text("angle bias:         Wald-F p = %.4f  (%s)", r.angle_test.p_value, r.angle_test.significant ? "kept" : "not significant");
        ImGui::Text("Raw Accel model:    Wald-F p = %.4f  (%s)", r.raw_accel_test.p_value,
                    r.raw_accel_test.significant ? "kept" : "not significant");
        ImGui::Text("speed curve:        Wald-F p = %.4f  (%s), sensitivity ∝ speed^%+.3f%s", r.speed_test.p_value,
                    r.speed_recommended ? "kept" : r.speed_test.significant ? "too small" : "not significant", r.speed_exponent,
                    r.speed_bounds_scaled ? " (scaled to the 5% bound)" : "");
        ImGui::Text("  flick levels %.2f–%.2f counts/ms: %+.1f%% / %+.1f%%; level bias %.1f%% → %.1f%% (in-sample)",
                    r.speed_slow_cpms, r.speed_fast_cpms, r.speed_slow_pct, r.speed_fast_pct, r.speed_level_bias_before_pct,
                    r.speed_level_bias_after_pct);
        ImGui::Text("bounds: ±%.0f%% sensitivity, ±%.1f° direction%s", Bounds{}.max_gain_deviation * 100, Bounds{}.max_angle_deg,
                    r.directional_bounds_scaled ? " — the fitted correction was scaled down to fit" : "");
        ui::pop_font();
    }
    int shown = 0;
    const std::string cal_id = ui_.calibration ? ui_.calibration->id : "";
    for (auto& m : app.setup().mouse_profiles) {
        if (m.status == "discarded") continue;
        ImGui::Dummy(ImVec2(0, 4));
        profile_card(app, m, m.calibration_session == cal_id ? &r : nullptr);
        ++shown;
    }
    if (!shown && r.ok)
        ui::muted("No candidate corrections. Your current setup is already consistent across directions within what "
                  "this calibration can resolve.");
}

void tab_validate(App& app) {
    auto& ui_ = app.mouse;
    auto& setup = app.setup();
    ui::begin_card("##mf_val", 0, 0);
    ui::card_title("Blinded comparison",
                   "Blocks alternate between your current setup and the candidate in a hidden, counterbalanced order "
                   "(ABBA BAAB). Targets use directions and distances that calibration never used. Speed and accuracy "
                   "are judged together; the default answer is to keep your baseline.");
    std::vector<MouseProfileRecord*> cands;
    for (auto& m : setup.mouse_profiles)
        if (m.status != "discarded") cands.push_back(&m);
    if (cands.empty()) {
        ui::faint("No candidate to validate.");
        ui::end_card();
        return;
    }
    MouseProfileRecord* sel = app.mouse_profile(ui_.selected_profile);
    if (!sel || sel->status == "discarded") sel = cands.front();
    ImGui::TextUnformatted("Candidate:");
    for (auto* m : cands)
        if (ImGui::RadioButton((m->label + "##" + m->id).c_str(), m == sel)) ui_.selected_profile = m->id;
    sel = app.mouse_profile(ui_.selected_profile) ? app.mouse_profile(ui_.selected_profile) : sel;
    ImGui::TextUnformatted("Length:");
    const char* lens[] = {"Standard: 8 blocks, 128 targets (about 2 min)", "Long: 12 blocks, 192 targets (about 3 min)",
                          "Extra long: 16 blocks, 256 targets (about 4 min)"};
    const int li = ui_.val_blocks >= 16 ? 2 : ui_.val_blocks >= 12 ? 1 : 0;
    for (int i = 0; i < 3; ++i)
        if (ImGui::RadioButton(lens[i], li == i)) ui_.val_blocks = i == 2 ? 16 : i == 1 ? 12 : 8;
    if (ui::button_primary("Start blinded comparison")) app.start_validation(sel->id);
    ui::end_card();

    if (!ui_.comparison) return;
    const ComparisonResult& c = *ui_.comparison;
    ui::section("Result");
    const bool good = c.verdict == Verdict::Improved;
    ui::callout(good ? ui::Tone::Good : c.verdict == Verdict::Worse ? ui::Tone::Bad : ui::Tone::Neutral,
                good ? "Candidate recommended" : c.verdict == Verdict::Worse ? "Candidate made things worse" :
                c.verdict == Verdict::InsufficientData ? "Not enough evidence" : "Keep your baseline",
                c.explanation.c_str());
    ui::begin_card("##mf_valres", 0, 0);
    const ui::Origin org = ui_.validation ? origin_of(ui_.validation->conditions.data_origin) : ui::Origin::Measured;
    if (ui_.validation) {
        int scored = 0;
        for (const auto& t : ui_.validation->trials) scored += t.spec.purpose == TrialPurpose::Scored ? 1 : 0;
        ui::muted("Blinded comparison · %s · %d scored targets", short_time(ui_.validation->finished_utc).c_str(), scored);
        ImGui::SameLine();
    }
    ui::origin_badge(org);
    ImGui::Dummy(ImVec2(0, 2));
    const float cw = ImGui::GetContentRegionAvail().x / 4.0f;
    const unsigned tc = c.verdict == Verdict::Improved ? theme::kGood : c.verdict == Verdict::Worse ? theme::kBad : theme::kText;
    ui::metric("aim time, candidate vs baseline", ui::fmt("%+.1f", (c.time_ratio - 1) * 100), "%", ui::Origin::None, tc);
    ImGui::SameLine(cw);
    ui::metric("95% range", ui::fmt("%+.1f … %+.1f", (c.time_ratio_ci.lo - 1) * 100, (c.time_ratio_ci.hi - 1) * 100), "%",
               ui::Origin::None);
    ImGui::SameLine(cw * 2);
    ui::metric("first-click hits, change", ui::fmt("%+.1f", c.hit_rate_diff * 100), "points", ui::Origin::None);
    ImGui::SameLine(cw * 3);
    ui::metric("consistent block pairs", ui::fmt("%d / %d", c.block_pairs_candidate_faster, c.block_pairs), "", ui::Origin::None);
    ImGui::Dummy(ImVec2(0, 4));
    for (const auto& d : c.details) ui::muted("%s", d.c_str());
    ui::faint("Aim time runs from the start of your movement to the hit, adjusted for target distance and direction, so "
              "reaction time does not add noise. Negative = faster.");
    ImGui::Dummy(ImVec2(0, 4));
    MouseProfileRecord* m = ui_.validation ? app.mouse_profile(ui_.validation->candidate_profile_id) : nullptr;
    if (m) {
        if (good) {
            if (ui::button_primary("Use this correction")) {
                setup.active_mouse_profile = m->id;
                for (auto& x : setup.mouse_profiles)
                    if (x.status == "active") x.status = "candidate";
                m->status = "active";
                app.save();
                ui_.tab = 4;
            }
            ImGui::SameLine();
        }
        if (ui::button_secondary("Keep my baseline")) {
            if (setup.active_mouse_profile == m->id) setup.active_mouse_profile.clear();
            app.save();
            app.toast(ui::Tone::Info, "Baseline kept. The candidate stays in History.");
        }
        ImGui::SameLine();
        if (ui::button_danger("Discard candidate")) {
            m->status = "discarded";
            if (setup.active_mouse_profile == m->id) setup.active_mouse_profile.clear();
            app.save();
        }
    }
    ui::end_card();
}

void tab_apply(App& app) {
    auto& setup = app.setup();
    auto& ui_ = app.mouse;
    MouseProfileRecord* active = app.mouse_profile(setup.active_mouse_profile);
    ui::begin_card("##mf_apply", 0, 0);
    ui::card_title("Applying a profile", "FitSuite never touches your mouse firmware or game files. A Raw Accel-compatible correction "
                                         "can be applied through Raw Accel, always with a way back.");
    if (!active) {
        ui::muted("No correction selected: your baseline (no correction) is in use.");
        ImGui::Dummy(ImVec2(0, 4));
        if (ImGui::BeginTable("##howto", 2, ImGuiTableFlags_SizingFixedFit)) {
            auto step = [](const char* n, const char* text) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ui::push_font(fonts().mono_medium, theme::kMono);
                ImGui::TextColored(theme::v4(theme::kAccent), "%s", n);
                ui::pop_font();
                ImGui::TableNextColumn();
                ui::wrapped(text, theme::kTextMuted);
            };
            step("1", "Run a blinded comparison (Validate). A correction is only offered here when it made you faster "
                      "without costing accuracy.");
            step("2", "Raw Accel-compatible corrections: FitSuite writes a new Raw Accel settings file next to your current "
                      "one; you load it in Raw Accel. Smooth directional corrections work inside FitSuite only.");
            step("3", "Going back: your original Raw Accel settings.json is never modified, so you can always return to it.");
            ImGui::EndTable();
        }
        ImGui::Dummy(ImVec2(0, 4));
        if (ui::button_secondary("Go to Validate")) ui_.tab = 3;
        ui::end_card();
        return;
    }
    ui::kv("Selected", active->label);
    ui::kv("Per direction", direction_summary(active->transform));
    if (active->validation) ui::kv("Validation", active->validation->explanation);
    staleness_line(app, active->conditions, "mousefit");
    ui::end_card();

    if (active->transform.kind == TransformKind::Directional) {
        ui::callout(ui::Tone::Warn, "In-app only",
                    "No existing driver can apply a smooth per-direction function to the raw input that VALORANT reads. "
                    "Raw Accel supports only rotation and per-axis ratios, mouse firmware at most separate X/Y DPI, and "
                    "input-injection tools are blocked by Vanguard. A custom signed driver would be needed (not built). "
                    "Use the Raw Accel-compatible candidate if you want something you can apply in games.");
        return;
    }
    const RawAccelProfile& p = active->transform.raw_accel;
    ui::begin_card("##mf_ra", 0, 0);
    ui::card_title("Raw Accel values", "Enter these in Raw Accel 1.7 (no acceleration), or let FitSuite write a new settings file for you.");
    if (ImGui::BeginTable("##ra", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 260.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        for (const auto& f : rawaccel_fields(p)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(theme::v4(theme::kTextMuted), "%s", f.gui_name.c_str());
            ImGui::TableNextColumn();
            ui::push_font(fonts().mono, theme::kMono);
            ImGui::TextUnformatted(f.value.c_str());
            ui::pop_font();
        }
        ImGui::EndTable();
    }
    ui::faint("Typing these in assumes Raw Accel currently has no rotation and ratios of 1. If you already use Raw Accel, "
              "let FitSuite create the settings file: it combines the correction with your current values.");
    if (ui::button_secondary("Copy values")) {
        std::string t;
        for (const auto& f : rawaccel_fields(p)) t += f.gui_name + ": " + f.value + "\n";
        ImGui::SetClipboardText(t.c_str());
        app.toast(ui::Tone::Info, "Copied.");
    }
    ImGui::SameLine();
    if (ui::button_primary("Create a Raw Accel settings file…")) {
        if (auto in = app.platform().open_file_dialog("Choose your Raw Accel settings.json", "Raw Accel settings", "*.json"))
            rawaccel_export(app, p, *in);
    }
    if (!ui_.patch_message.empty()) ui::wrapped(ui_.patch_message.c_str(), theme::kTextMuted);
    for (const auto& w : ui_.patch_warnings) ui::callout(ui::Tone::Warn, nullptr, w.c_str());
    if (!ui_.ra_generated.empty() && setup.fix_games.enabled) {
        ImGui::Dummy(ImVec2(0, 4));
        ui::callout(ui::Tone::Info, "The fix switches per game",
                    "FitSuite turns the fix on and off in Raw Accel as your games come to the front (Mouse tab, \"Use it in your games\"). "
                    "Turn that off there before applying a settings file by hand.");
    } else if (!ui_.ra_generated.empty()) {
        ImGui::Dummy(ImVec2(0, 4));
        if (!app.platform().rawaccel_driver_present())
            ui::callout(ui::Tone::Warn, "Raw Accel's driver was not found",
                        "Install Raw Accel first (administrator rights and a restart), then apply.");
        ImGui::BeginDisabled(app.rawaccel_busy());
        if (ui::button_primary("Apply now in Raw Accel")) {
            AppliedChange ch;
            ch.id = make_id("ch");
            ch.kind = "mousefit.rawaccel_apply";
            ch.target = ui_.ra_generated.string();
            ch.before = ui_.ra_original.string();
            ch.after = ui_.ra_generated.string();
            ch.applied_utc = utc_now_iso();
            ch.status = "pending";
            std::string err;
            if (app.rawaccel_run_writer(ui_.ra_generated, "apply", ch.id, &err)) {
                setup.applied_changes.push_back(ch);
                app.save();
            } else {
                ui_.patch_message = err;
            }
        }
        ImGui::SameLine();
        if (ui::button_secondary("Make it my Raw Accel default…")) ui_.confirm_default = !ui_.confirm_default;
        ImGui::SameLine();
        if (ui::button_secondary("Restore my previous settings")) {
            AppliedChange* last = nullptr;
            for (auto& c : setup.applied_changes)
                if ((c.kind == "mousefit.rawaccel_apply" || c.kind == "mousefit.rawaccel_default") && c.status == "applied") last = &c;
            std::string err;
            if (last) {
                if (!app.rawaccel_restore(*last, &err)) ui_.patch_message = err;
            } else if (!app.rawaccel_run_writer(ui_.ra_original, "restore", "", &err)) {
                ui_.patch_message = err;
            }
        }
        ImGui::EndDisabled();
        if (app.rawaccel_busy()) {
            ImGui::SameLine();
            ui::faint("Raw Accel is applying…");
        }
        if (ui_.confirm_default) {
            ui::callout(ui::Tone::Neutral, "Make the correction your Raw Accel default?",
                        "FitSuite first saves a copy of your settings.json next to it, then puts the corrected settings in its "
                        "place and applies them, so they stay active after a restart. \"Restore my previous settings\" puts "
                        "your copy back.");
            ImGui::BeginDisabled(app.rawaccel_busy());
            if (ui::button_primary("Make it my default")) {
                std::string msg;
                app.rawaccel_make_default(ui_.ra_original, ui_.ra_generated, &msg);
                ui_.patch_message = msg;
                ui_.confirm_default = false;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ui::button_secondary("Cancel")) ui_.confirm_default = false;
        }
        ui::faint("\"Apply now\" lasts until Windows restarts or Raw Accel loads settings.json again. Raw Accel applies the "
                  "correction to all mouse movement on this PC, in every game, based only on which way you move.");
    }
    ImGui::Dummy(ImVec2(0, 4));
    ui::faint("Raw Accel is a separate, signed driver (MIT licence). Its FAQ says it works with VALORANT's anti-cheat; "
              "FitSuite has not verified this. Installing it needs administrator rights and a restart.");
    ui::end_card();

    ui::begin_card("##mf_restore", 0, 0);
    ui::card_title("Go back", "FitSuite keeps a copy of your Raw Accel settings before it changes anything, so you can always "
                              "return to your previous mouse feel (Restore above, or History → Changes made by FitSuite).");
    if (ui::button_secondary("Use baseline in FitSuite (no correction)")) {
        setup.active_mouse_profile.clear();
        active->status = "candidate";
        app.save();
    }
    ui::end_card();
}

}  // namespace

std::vector<GameChoice> game_choices(const std::vector<std::string>& selected) {
    std::vector<GameChoice> out;
    for (const auto& k : known_shooters()) {
        auto it = std::find_if(out.begin(), out.end(), [&](const GameChoice& c) { return c.name == k.name; });
        if (it == out.end()) out.push_back({k.name, {k.exe}, k.yaw});
        else it->exes.push_back(k.exe);
    }
    for (const auto& e : selected) {
        bool known = false;
        for (const auto& c : out)
            known = known || std::find(c.exes.begin(), c.exes.end(), e) != c.exes.end();
        if (!known) out.push_back({game_display_name(e), {e}, 0.0});
    }
    return out;
}

std::string main_game_name(const SetupProfile& s) { return game_display_name(main_game_exe(s.game.id)); }

double main_game_cm360(const SetupProfile& s) {
    const double yaw = game_yaw(main_game_exe(s.game.id));
    if (yaw > 0.0) return cm_per_360(yaw, s.game.sensitivity, s.mouse.dpi);
    return s.game.cm360 > 0.0 ? s.game.cm360 : 0.0;
}

bool main_game_ready(const SetupProfile& s) {
    if (s.mouse.dpi <= 0) return false;
    return game_yaw(main_game_exe(s.game.id)) > 0.0 ? s.game.sensitivity > 0.0 : s.game.cm360 > 0.0;
}

bool is_main_game(const SetupProfile& s, const GameChoice& g) {
    const std::string main = main_game_exe(s.game.id);
    return std::find(g.exes.begin(), g.exes.end(), main) != g.exes.end();
}

double main_game_hfov(const SetupProfile& s) {
    const double fixed = game_fixed_hfov(main_game_exe(s.game.id));
    if (fixed > 0.0) return fixed;
    return s.game.hfov > 0.0 ? s.game.hfov : 103.0;
}

GameMapping main_game_mapping(const SetupProfile& s) {
    GameMapping m;
    m.game_id = s.game.id;
    m.hfov_deg_16x9 = main_game_hfov(s);  // the test shows the same view as the game
    const double yaw = game_yaw(main_game_exe(s.game.id));
    if (yaw > 0.0) {
        m.deg_per_count_at_sens1 = yaw;
        m.sensitivity = s.game.sensitivity;
    } else if (s.game.cm360 > 0.0 && s.mouse.dpi > 0) {
        // No published turn rate: the distance for a full turn fixes degrees per count directly.
        m.deg_per_count_at_sens1 = 360.0 / (s.game.cm360 / 2.54 * double(s.mouse.dpi));
        m.sensitivity = 1.0;
    } else {
        m.sensitivity = s.game.sensitivity;  // not set up yet: VALORANT's scale
    }
    return m;
}

void main_game_sens_input(App& app, float width) {
    auto& s = app.setup();
    const std::string exe = main_game_exe(s.game.id);
    ImGui::SetNextItemWidth(width);
    if (game_yaw(exe) > 0.0) {
        float sens = float(s.game.sensitivity);
        if (ImGui::InputFloat("##sens", &sens, 0.0f, 0.0f, "%.3f")) {
            s.game.sensitivity = std::clamp(double(sens), 0.001, 100.0);
            app.save();
        }
    } else {
        float cm = float(s.game.cm360);
        if (ImGui::InputFloat("##cm360", &cm, 0.0f, 0.0f, cm > 0 ? "%.1f" : "")) {
            s.game.cm360 = std::clamp(double(cm), 0.0, 10000.0);
            app.save();
        }
        ImGui::SameLine();
        ui::faint("cm per 360°");
        ui::help_marker("FitSuite cannot convert this game's sensitivity (its scale depends on the field of view). Measure "
                        "how far you move the mouse for one full turn in the game, or use a sensitivity converter, and "
                        "enter the distance.");
    }
    // The test draws the game's view: fixed for VALORANT and Counter-Strike 2, a setting elsewhere.
    if (game_fixed_hfov(exe) <= 0.0) {
        float fov = float(s.game.hfov > 0.0 ? s.game.hfov : 103.0);
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputFloat("##hfov", &fov, 0.0f, 0.0f, "%.0f")) {
            s.game.hfov = std::clamp(double(fov), 50.0, 170.0);
            app.save();
        }
        ImGui::SameLine();
        ui::faint("° field of view");
        ui::help_marker("So FitSuite's test shows targets where your game would. Enter the horizontal field of view at "
                        "16:9 (103 if you are not sure). Some games show a different number in their settings, such as "
                        "the 4:3 value; a field-of-view converter gives the 16:9 one.");
    }
}

std::string main_game_sens_label(const SetupProfile& s) {
    const std::string exe = main_game_exe(s.game.id);
    return game_yaw(exe) > 0.0 ? main_game_name(s) + " sensitivity" : main_game_name(s) + " full turn";
}

void main_game_picker(App& app) {
    auto& s = app.setup();
    const std::string current = main_game_exe(s.game.id);
    const auto choices = game_choices(s.fix_games.games);
    if (ImGui::BeginTable("##main_game", 3, ImGuiTableFlags_SizingStretchSame)) {
        for (const auto& c : choices) {
            ImGui::TableNextColumn();
            const bool on = std::find(c.exes.begin(), c.exes.end(), current) != c.exes.end();
            if (ImGui::RadioButton(c.name.c_str(), on) && !on) {
                // Keep the turn distance: the new game's sensitivity is filled in so a full turn takes
                // the same mouse distance as before (the player checks it against the real value).
                const double cm = main_game_cm360(s);
                const std::string exe = c.exes.front();
                s.game.id = game_id_for_exe(exe);
                const double yaw = game_yaw(exe);
                if (yaw > 0.0 && cm > 0.0 && s.mouse.dpi > 0) s.game.sensitivity = 360.0 * 2.54 / (yaw * cm * double(s.mouse.dpi));
                else if (yaw <= 0.0) s.game.cm360 = cm;
                bool ticked = false;  // the main game is one of "your games"
                for (const auto& e : c.exes)
                    ticked = ticked || std::find(s.fix_games.games.begin(), s.fix_games.games.end(), e) != s.fix_games.games.end();
                if (!ticked) s.fix_games.games.push_back(exe);
                app.save();
            }
        }
        ImGui::EndTable();
    }
}

double game_cm360(const SetupProfile& s, const GameChoice& g) {
    if (is_main_game(s, g)) return main_game_cm360(s);
    auto it = s.app.game_sens.find(g.exes.front());
    if (it == s.app.game_sens.end()) return 0.0;
    if (g.yaw > 0.0 && it->second.sensitivity > 0.0) return cm_per_360(g.yaw, it->second.sensitivity, s.mouse.dpi);
    return it->second.cm360 > 0.0 ? it->second.cm360 : 0.0;
}

std::string turn_comparison(double game_cm, double main_cm, const std::string& main_name, bool* warn, bool brief) {
    if (warn) *warn = false;
    if (!(game_cm > 0.0) || !(main_cm > 0.0)) return "";
    const double r = main_cm / game_cm;  // > 1: the game turns faster (less distance for a full turn)
    if (std::fabs(r - 1.0) <= 0.10) return brief ? "about the same" : "about the same as " + main_name;
    if (warn) *warn = std::fabs(r - 1.0) > 0.15;
    return ui::fmt("turns %.0f%% %s%s", std::fabs(r - 1.0) * 100.0, r > 1.0 ? "faster" : "slower",
                   brief ? "" : (" than " + main_name).c_str());
}

// Writes settings.fitsuite-<time>.json next to the player's Raw Accel settings.json (which is not
// changed): their profile with the correction composed into its rotation and ratios.
bool rawaccel_export(App& app, const RawAccelProfile& correction, const std::filesystem::path& settings_json) {
    auto& ui_ = app.mouse;
    std::string err;
    auto text = read_file(settings_json, &err, 8u << 20);
    if (!text) {
        ui_.patch_message = err;
        return false;
    }
    auto r = patch_rawaccel_settings(*text, correction, "", &err);
    if (!r) {
        ui_.patch_message = err;
        return false;
    }
    const auto out = settings_json.parent_path() / ("settings.fitsuite-" + utc_compact_now() + ".json");
    if (!atomic_write_file(out, r->json, &err)) {
        ui_.patch_message = err;
        return false;
    }
    ui_.patch_message = "Wrote " + out.filename().string() + " next to your settings.json, which was not changed.";
    ui_.patch_warnings = r->warnings;
    ui_.ra_original = settings_json;
    ui_.ra_generated = out;
    AppliedChange ch;
    ch.id = make_id("ch");
    ch.kind = "mousefit.rawaccel_export";
    ch.target = out.string();
    ch.before = settings_json.string();
    ch.after = out.string();
    ch.applied_utc = utc_now_iso();
    ch.status = "applied";
    app.setup().applied_changes.push_back(ch);
    app.save();
    return true;
}

// Full MouseFit workflow with every statistic (Nerd stats → MouseFit).
void mousefit_details(App& app) {
    static const char* labels[5] = {"Setup", "Calibrate", "Candidates", "Validate", "Apply"};
    bool done[5] = {main_game_ready(app.setup()), app.mouse.calibration.has_value(),
                    app.mouse.fit && app.mouse.fit->ok, app.mouse.comparison.has_value(),
                    !app.setup().active_mouse_profile.empty()};
    const int clicked = ui::steps(labels, 5, app.mouse.tab, done);
    if (clicked >= 0) app.mouse.tab = clicked;
    ImGui::Dummy(ImVec2(0, 4));
    switch (app.mouse.tab) {
        case 0: tab_setup(app); break;
        case 1: tab_calibrate(app); break;
        case 2: tab_candidates(app); break;
        case 3: tab_validate(app); break;
        default: tab_apply(app); break;
    }
}

// ---- simple view ---------------------------------------------------------------------------------
namespace {

// The correction that can be used in games: the Raw Accel-compatible candidate from the latest calibration.
MouseProfileRecord* game_fix(App& app) {
    const std::string cal = app.mouse.calibration ? app.mouse.calibration->id : std::string();
    MouseProfileRecord* found = nullptr;
    for (auto& m : app.setup().mouse_profiles) {
        if (m.status == "discarded" || m.transform.kind != TransformKind::RawAccel) continue;
        if (!cal.empty() && m.calibration_session != cal) continue;
        found = &m;
    }
    return found;
}

// "Your down flicks land 9% shorter than your average flick." from the calibration summary.
std::string flick_story(const std::vector<DirectionSummary>& dirs) {
    static const char* names[8] = {"right", "up-right", "up", "up-left", "left", "down-left", "down", "down-right"};
    // Compare each direction with the player's typical direction (median), so one bad direction
    // stands out instead of making all the others look "long".
    std::vector<double> v;
    for (const auto& d : dirs)
        if (d.n) v.push_back(d.mean_radial_error_pct);
    if (v.empty()) return "Not enough flicks to describe yet.";
    const double avg = median(v);
    std::vector<std::pair<double, int>> dev;
    for (int i = 0; i < int(dirs.size()) && i < 8; ++i)
        if (dirs[size_t(i)].n && std::fabs(dirs[size_t(i)].mean_radial_error_pct - avg) >= 4.0)
            dev.push_back({dirs[size_t(i)].mean_radial_error_pct - avg, i});
    std::sort(dev.begin(), dev.end(), [](const auto& a, const auto& b) { return std::fabs(a.first) > std::fabs(b.first); });
    std::string out;
    for (size_t k = 0; k < dev.size() && k < 3; ++k) {
        const auto& [d, i] = dev[k];
        out += (out.empty() ? "Your " : ", ") + std::string(names[i]) + " flicks land " + ui::fmt("%.0f%%", std::fabs(d)) +
               (d < 0 ? " shorter" : " further");
    }
    if (!out.empty()) out += " than your typical flick.";
    int worst = -1;
    for (int i = 0; i < int(dirs.size()) && i < 8; ++i)
        if (dirs[size_t(i)].n && std::fabs(dirs[size_t(i)].mean_angle_error_deg) >= 2.0 &&
            (worst < 0 || std::fabs(dirs[size_t(i)].mean_angle_error_deg) > std::fabs(dirs[size_t(worst)].mean_angle_error_deg)))
            worst = i;
    if (worst >= 0) {
        const double a = dirs[size_t(worst)].mean_angle_error_deg;
        out += (out.empty() ? "" : " ") + std::string("Your ") + names[worst] + " flicks drift " + ui::fmt("%.0f°", std::fabs(a)) +
               (a > 0 ? " counter-clockwise." : " clockwise.");
    }
    return out.empty() ? "Your flicks land about the same in every direction." : out;
}

// One click: corrected copy of the player's Raw Accel settings → saved copy of settings.json →
// corrected settings in its place → applied with Raw Accel's writer.
void use_in_rawaccel(App& app, const RawAccelProfile& fix, bool choose_again) {
    std::string note;
    std::filesystem::path sj = choose_again ? std::filesystem::path() : app.rawaccel_settings_ready(&note);
    if (sj.empty()) {
        auto picked = app.platform().open_file_dialog("Choose settings.json in your Raw Accel folder", "Raw Accel settings", "settings.json");
        if (!picked) return;
        sj = *picked;
        app.remember_rawaccel_settings_path(sj);
    }
    if (!rawaccel_export(app, fix, sj)) return;
    std::string msg;
    app.rawaccel_make_default(sj, app.mouse.ra_generated, &msg);
    app.mouse.patch_message = note.empty() ? msg : note + " " + msg;
}

const AppliedChange* active_rawaccel_change(const SetupProfile& s) {
    const AppliedChange* last = nullptr;
    for (const auto& c : s.applied_changes)
        if ((c.kind == "mousefit.rawaccel_apply" || c.kind == "mousefit.rawaccel_default") && c.status == "applied") last = &c;
    return last;
}

}  // namespace

namespace {

// Label column + field column, like the rows of a Windows settings form.
bool begin_form(const char* id, float label_w = 170.0f) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, label_w);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void form_label(const char* text) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::TableNextColumn();
}

// Turns the per-game fix on with the remembered Raw Accel settings.json (asks for it the first time).
void turn_on_in_games(App& app) {
    std::string note;
    std::filesystem::path sj = app.rawaccel_settings_ready(&note);
    if (sj.empty())
        if (auto picked = app.platform().open_file_dialog("Choose settings.json in your Raw Accel folder", "Raw Accel settings",
                                                          "settings.json"))
            sj = *picked;
    if (sj.empty()) return;
    std::string msg;
    if (app.games_turn_on(sj, &msg)) app.remember_rawaccel_settings_path(sj);
    app.mouse.patch_message = note.empty() ? msg : msg.empty() ? note : note + " " + msg;
}

void choose_fix(App& app, MouseProfileRecord& fix) {
    auto& setup = app.setup();
    setup.active_mouse_profile = fix.id;
    for (auto& x : setup.mouse_profiles)
        if (x.status == "active") x.status = "candidate";
    fix.status = "active";
    app.save_now();
}

void games_box(App& app, MouseProfileRecord* fix, float w);

}  // namespace

void page_mousefit(App& app) {
    auto& setup = app.setup();
    auto& ui_ = app.mouse;
    ui::page_header("MouseFit", "Measures whether your flicks land differently in some directions and fixes that through Raw "
                                "Accel, but only if a blind test shows the fix makes you faster.");
    refresh_rate_notice(app);
    const float avail = ImGui::GetContentRegionAvail().x;
    const bool two_cols = avail >= 900.0f;
    const float gap = 14.0f;
    const float lw = two_cols ? std::floor(std::min(520.0f, (avail - gap) * 0.47f)) : avail;
    const float rw = two_cols ? avail - lw - gap : avail;

    const bool s1 = main_game_ready(setup);
    const bool s2 = ui_.calibration.has_value();
    MouseProfileRecord* fix = game_fix(app);
    const bool fit_ok = ui_.fit && ui_.fit->ok;
    std::vector<DirectionSummary> dirs;
    if (s2) dirs = summarize_by_direction(ui_.calib_metrics, 8);

    // ---- left column ----
    ImGui::BeginGroup();
    ui::begin_card("##mf_mouse", lw, 0);
    ui::card_title("Your mouse");
    if (begin_form("##mf_fields")) {
        form_label("DPI");
        int dpi = setup.mouse.dpi;
        ImGui::SetNextItemWidth(110);
        if (ImGui::InputInt("##dpi", &dpi, 0, 0)) {
            setup.mouse.dpi = std::clamp(dpi, 50, 64000);
            app.save();
        }
        form_label(main_game_sens_label(setup).c_str());
        main_game_sens_input(app, 110);
        form_label("Mouse");
        ImGui::TextUnformatted(!setup.mouse.model.empty() ? setup.mouse.model.c_str()
                               : !setup.mouse.name.empty() ? setup.mouse.name.c_str()
                                                           : "Not set (Settings)");
        ui::help_marker(("Use the DPI from your mouse software and your sensitivity from " + main_game_name(setup) +
                         " (" + game_sens_where(main_game_exe(setup.game.id)) +
                         "). Change your main game or type your mouse model in Settings. To pick a specific mouse for "
                         "the tests, see Nerd stats → Mouse → Setup.")
                            .c_str());
        ImGui::EndTable();
    }
    if (setup.mouse_sessions.empty() && setup.mouse.dpi == 800 && std::fabs(setup.game.sensitivity - 0.4) < 1e-9)
        ui::faint("800 and 0.400 are only examples: enter your own, so the test feels like your game.");
    ui::end_card();

    // 1 — measure, see the fix, and its blind test, in one box. A fix found by the measurement goes to
    // its blind test by itself, and one that passes is switched on (App::on_mouse_test_done).
    ui::begin_card("##mf_measure", lw, 0);
    ui::card_title("1  Measure and fix");
    // What the chosen mouse's published specs say about the DPI entered and the report rate measured,
    // here where measuring starts, so a wrong DPI is fixed before it goes into a measurement.
    if (const KnownMouse* km = find_mouse(setup.mouse.model)) {
        const MouseAdvice adv = mouse_advice(*km, setup.mouse.dpi, setup.mouse.polling_hz);
        for (const auto& w : adv.warnings) ui::wrapped(w.c_str(), theme::kWarn);
        for (const auto& n : adv.notes) ui::wrapped(n.c_str(), theme::kTextMuted);
    }
    ImGui::BeginDisabled(!s1);
    if (!s2) {
        ui::wrapped("Five short rounds, about 4 minutes in all: flicks, micro-adjustments, tracking and target switching, "
                    "like an aim trainer. If your flicks need a fix, a 2-minute blind test follows, and a fix that makes "
                    "you faster is switched on in your games. Full screen; Esc stops, switching windows pauses.",
                    theme::kTextMuted);
        if (ui::button_primary("Measure my flicks")) app.start_calibration();
        if (!s1) {
            ImGui::SameLine();
            ui::faint("Enter your DPI and sensitivity first.");
        }
    } else {
        const Session& c = *ui_.calibration;
        ui::wrapped(flick_story(dirs).c_str());
        int scored = 0;
        for (const auto& t : c.trials) scored += t.spec.purpose == TrialPurpose::Scored ? 1 : 0;
        ui::faint("Measured %s, %d flicks", short_time(c.finished_utc).c_str(), scored);
        ImGui::SameLine();
        ui::origin_badge(origin_of(c.conditions.data_origin));
        if (ui_.aim_report && !ui_.aim_report->lines.empty()) {
            ImGui::Dummy(ImVec2(0, 2));
            for (const auto& l : ui_.aim_report->lines) ui::wrapped(l.c_str(), theme::kTextMuted);
        }
        int64_t when = 0;
        if (parse_utc_iso(c.finished_utc, when)) {
            const int64_t days = (unix_now() - when) / 86400;
            if (days >= 30)
                ui::callout(ui::Tone::Warn, ui::fmt("Measured %lld days ago", (long long)days).c_str(),
                            "Aim habits drift. Measure again to keep your fix up to date.");
        }
        staleness_line(app, [&] {
            Conditions rec;
            rec["mouse.dpi"] = std::to_string(c.conditions.dpi);
            rec["game.sensitivity"] = ui::fmt("%.6g", c.conditions.mapping.sensitivity);
            return rec;
        }(), "mousefit");
        if (ui::button_secondary("Measure again")) app.start_calibration();
    }
    ImGui::EndDisabled();

    // Your fix (part of box 1).
    if (s2) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Separator();
        ui::push_font(fonts().semibold, theme::kBody);
        ImGui::TextUnformatted("Your fix");
        ui::pop_font();
    }
    if (!s2) {
        // nothing yet
    } else if (!fit_ok) {
        ui::wrapped(ui_.fit ? ui_.fit->message.c_str() : "Not enough data.", theme::kTextMuted);
    } else if (!fix) {
        ui::wrapped("No fix needed: your flicks land about the same in every direction. Keep your current setup.");
    } else {
        const RawAccelProfile& ra = fix->transform.raw_accel;
        if (begin_form("##mf_fixvals", 170.0f)) {
            struct D { const char* name; double deg; };
            static const D rows[4] = {{"Up", 90}, {"Down", -90}, {"Left", 180}, {"Right", 0}};
            for (const auto& d : rows) {
                form_label(ui::fmt("Sensitivity %s", d.name).c_str());
                double pct = (ra.gain(d.deg * kDegToRad) - 1.0) * 100.0;
                if (std::fabs(pct) < 0.05) pct = 0.0;  // no "-0.0%"
                ImGui::TextColored(theme::v4(std::fabs(pct) < 0.5 ? theme::kTextMuted : theme::kText), "%+.1f%%", pct);
            }
            form_label("Rotation");
            const double rot = std::fabs(ra.rotation_deg) < 0.05 ? 0.0 : ra.rotation_deg;
            ImGui::TextColored(theme::v4(rot == 0.0 ? theme::kTextMuted : theme::kText), rot == 0.0 ? "none" : "%+.1f°", rot);
            if (!ra.speed.empty() && ra.speed.points.size() >= 3) {
                // make_speed_curve spans 0.1 × the slowest to 1.5 × the fastest measured flick speed.
                const double v_slow = ra.speed.points.front().first / 0.1;
                const double v_fast = ra.speed.points[ra.speed.points.size() - 2].first / 1.5;
                const double slow = (speed_curve_flick_gain(ra.speed, v_slow) - 1.0) * 100.0;
                const double fast = (speed_curve_flick_gain(ra.speed, v_fast) - 1.0) * 100.0;
                form_label("With hand speed");
                ImGui::Text("%+.1f%% near (slow) flicks, %+.1f%% far (fast) flicks", slow, fast);
            }
            ImGui::EndTable();
        }
        ui::faint("Your average sensitivity stays the same, and no direction changes by more than 8%%.");
    }

    // Blind test (part of box 1).
    const ComparisonResult* cmp =
        ui_.comparison && fix && ui_.validation && ui_.validation->candidate_profile_id == fix->id ? &*ui_.comparison : nullptr;
    const std::string verdict = cmp ? to_string(cmp->verdict) : fix && fix->validation ? fix->validation->verdict : std::string();
    const double ratio = cmp ? cmp->time_ratio : fix && fix->validation ? fix->validation->time_ratio : 1.0;
    const bool improved = verdict == "improved";
    const bool chosen = fix && setup.active_mouse_profile == fix->id;
    if (!fix) {
        // no fix: nothing to test
    } else if (verdict.empty()) {
        ImGui::Dummy(ImVec2(0, 2));
        ui::wrapped("Not blind-tested yet. Eight short flick blocks (about 2 minutes): FitSuite switches between your "
                    "current setup and the fix without telling you which is on, and uses the fix only if you hit targets "
                    "faster without missing more.",
                    theme::kTextMuted);
        if (ui::button_primary("Start the blind test")) app.start_validation(fix->id);
    } else {
        ImGui::Dummy(ImVec2(0, 2));
        ui::push_font(fonts().semibold, theme::kBody);
        if (improved) ImGui::TextColored(theme::v4(theme::kGood), "%.0f%% faster with the fix, and just as accurate.", (1.0 - ratio) * 100.0);
        else if (verdict == "worse") ImGui::TextColored(theme::v4(theme::kBad), "The fix made you slower. Keep your current setup.");
        else ImGui::TextUnformatted("No clear difference. Keep your current setup.");
        ui::pop_font();
        const bool in_games = setup.fix_games.enabled && chosen;
        if (improved && !in_games) {
            if (app.rawaccel_installed()) {
                ImGui::BeginDisabled(app.rawaccel_busy() || setup.fix_games.games.empty());
                if (ui::button_primary("Use this fix in my games")) {
                    choose_fix(app, *fix);
                    turn_on_in_games(app);
                }
                ImGui::EndDisabled();
            } else if (!chosen) {
                if (ui::button_primary("Use this fix")) choose_fix(app, *fix);
            }
            ImGui::SameLine();
        }
        if (!improved && verdict != "worse") {
            if (ui::button_secondary("Test again (longer)")) {
                ui_.val_blocks = 16;
                app.start_validation(fix->id);
            }
        } else if (ui::button_secondary("Test again")) {
            app.start_validation(fix->id);
        }
    }

    // The quick check lives in Settings → Your mouse now; only its reminder and a changed verdict show here.
    if (MouseProfileRecord* used = app.active_fix()) {
        const bool changed = used->last_check && used->last_check->verdict == "changed";
        if (changed) {
            ui::callout(ui::Tone::Warn, "Your aim has changed", used->last_check->explanation.c_str());
        } else if (app.check_due()) {
            ui::callout(ui::Tone::Info, "Time for a quick check",
                        ui::fmt("Your fix was last measured or checked %d days ago. About 1 minute.",
                                app.days_since_fix_checked()).c_str());
            if (ui::button_secondary("Quick check")) app.start_check();
        }
    }
    ui::end_card();
    ImGui::EndGroup();

    // ---- right column ----
    if (two_cols) ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ui::begin_card("##mf_chart", rw, 0);
    ui::card_title("Your flicks by direction");
    if (s2) {
        const float size = std::min(rw - 26.0f, 330.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (rw - 26.0f - size) * 0.5f));
        direction_wheel(dirs, size);
        ui::faint("Bars: how far your flicks land past (orange) or short of (teal) the target, which is the solid circle; "
                  "numbers in %%. Dashed circle: your average. Dots: sideways drift, drawn 4 times larger.");
    } else {
        ui::faint("Measure your flicks to see where they land.");
    }
    ui::end_card();
    games_box(app, fix, rw);
    ImGui::EndGroup();
}

namespace {

// 4 — the fix in games: switched on in Raw Accel while one of the ticked games is in front.
void games_box(App& app, MouseProfileRecord* fix, float w) {
    auto& setup = app.setup();
    auto& ui_ = app.mouse;
    auto& fg = setup.fix_games;
    const bool chosen = fix && setup.active_mouse_profile == fix->id;
    const App::GamesState gs = app.games_state();
    const AppliedChange* everywhere = active_rawaccel_change(setup);
    const bool driver = app.rawaccel_installed();
    const bool usable = chosen || fg.enabled || everywhere;

    ui::begin_card("##mf_games", w, 0);
    ui::card_title("2  Use it in your games");
    if (!driver && !fg.enabled) {
        ui::wrapped("Needs Raw Accel, a free signed mouse driver (administrator rights and a restart to install).",
                    theme::kTextMuted);
        if (ui::button_secondary("Install Raw Accel…")) {  // the setup's Raw Accel step does it
            app.setup().app.setup_done = false;
            app.setup().app.setup_step = 3;
            app.save();
        }
    } else {
        ui::wrapped("The fix is switched on only while one of these games is in front, and off for everything else. It works "
                    "on top of each game's own sensitivity.", theme::kTextMuted);
    }
    if (!usable) ui::faint("Pass the blind test first, then turn it on here.");

    // Programs with a window, for "running" marks and adding a game (refreshed every 3 seconds).
    if (ui_.running_checked_at == 0.0 || app.time() - ui_.running_checked_at > 3.0) {
        ui_.running = app.platform().running_apps();
        ui_.running_checked_at = app.time();
    }
    auto is_running = [&](const std::string& exe) {
        for (const auto& r : ui_.running)
            if (normalize_exe(r.first) == exe) return true;
        return false;
    };
    auto selected = [&](const std::string& exe) { return std::find(fg.games.begin(), fg.games.end(), exe) != fg.games.end(); };

    ImGui::Dummy(ImVec2(0, 2));
    const auto choices = game_choices(fg.games);
    const int cols = w > 560 ? 3 : 2;
    if (ImGui::BeginTable("##games", cols, ImGuiTableFlags_SizingStretchSame)) {
        for (const auto& c : choices) {
            ImGui::TableNextColumn();
            ImGui::PushID(c.exes.front().c_str());
            bool on = false, running = false;
            for (const auto& e : c.exes) {
                on = on || selected(e);
                running = running || is_running(e);
            }
            if (ImGui::Checkbox(c.name.c_str(), &on)) {
                for (const auto& e : c.exes) {
                    fg.games.erase(std::remove(fg.games.begin(), fg.games.end(), e), fg.games.end());
                    if (on) fg.games.push_back(e);
                }
                app.save();
            }
            if (running) {
                ImGui::SameLine();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().FramePadding.y);
                ui::pill("running", theme::kGood);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    {
        bool all = true;
        for (const auto& k : known_shooters()) all = all && selected(k.exe);
        if (ui::button_secondary(all ? "Untick all" : "Tick all")) {
            for (const auto& k : known_shooters()) {
                fg.games.erase(std::remove(fg.games.begin(), fg.games.end(), std::string(k.exe)), fg.games.end());
                if (!all) fg.games.push_back(k.exe);
            }
            app.save();
        }
        ImGui::SameLine();
        if (ui::button_secondary(ui_.show_running ? "Close list" : "Add a running game…")) ui_.show_running = !ui_.show_running;
        ui::help_marker(("Your fix is measured with your " + main_game_name(setup) +
                         " setting, so it suits shooters where a full turn takes about the same mouse distance. FitSuite "
                         "only reads the name of the program in front (for example VALORANT-Win64-Shipping.exe); it never "
                         "looks into a game, its screen or its memory.")
                            .c_str());
    }
    if (ui_.show_running) {
        int shown = 0;
        const float row = ImGui::GetTextLineHeightWithSpacing();
        if (ImGui::BeginListBox("##running", ImVec2(-1, row * 5.25f))) {
            for (const auto& [exe, title] : ui_.running) {
                const std::string e = normalize_exe(exe);
                if (selected(e)) continue;
                ++shown;
                if (ImGui::Selectable((title + "   (" + e + ")##" + e).c_str())) {
                    fg.games.push_back(e);
                    app.save();
                }
            }
            if (!shown) ImGui::TextDisabled("No other programs with a window. Start the game, then come back.");
            ImGui::EndListBox();
        }
        ui::faint("Click a program to add it.");
    }

    // Turn speed of the ticked games compared with the main-game setting the fix was measured at.
    {
        const double val_cm = main_game_cm360(setup);
        const std::string main_name = main_game_name(setup);
        int unknown = 0;
        for (const auto& c : choices) {
            bool on = false;
            for (const auto& e : c.exes) on = on || selected(e);
            if (!on || is_main_game(setup, c)) continue;
            bool warn = false;
            const std::string cmp = turn_comparison(game_cm360(setup, c), val_cm, main_name, &warn);
            if (cmp.empty()) ++unknown;
            else if (warn)
                ui::wrapped(ui::fmt("%s %s, so the fix may fit it less well.", c.name.c_str(), cmp.c_str()).c_str(), theme::kWarn);
        }
        if (unknown)
            ui::faint("Enter your other games' sensitivity in Settings to see whether they turn like your %s setting.",
                      main_name.c_str());
    }

    bool sw = setup.app.start_with_windows;
    if (ImGui::Checkbox("Open FitSuite when Windows starts (it waits in the notification area)", &sw))
        app.set_start_with_windows(sw);
    ui::help_marker("Without this, the fix only switches while FitSuite runs. Closing the window keeps FitSuite running in "
                    "the notification area; right-click its icon there to quit. The same setting is in Settings.");

    ImGui::BeginDisabled(app.rawaccel_busy());
    if (!fg.enabled) {
        ImGui::BeginDisabled(!driver || !chosen || fg.games.empty());
        if (ui::button_primary("Turn on")) turn_on_in_games(app);
        ImGui::EndDisabled();
    } else {
        if (!gs.error.empty()) {
            if (ui::button_primary("Try again")) app.games_retry();
            ImGui::SameLine();
        }
        if (ui::button_secondary("Turn off")) {
            app.games_turn_off();
            ui_.patch_message = "Off. Raw Accel is back on your own settings, and FitSuite no longer starts with Windows.";
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, 14);
    // Plain status next to the button.
    if (fg.enabled) {
        if (!gs.error.empty()) ImGui::TextColored(theme::v4(theme::kWarn), "Paused");
        else if (!gs.have_fix) ImGui::TextColored(theme::v4(theme::kWarn), "No fix chosen, so it stays off");
        else if (gs.switching) ui::muted("Switching Raw Accel…");
        else if (gs.fix_on == true) ImGui::TextColored(theme::v4(theme::kGood), "On in %s", game_display_name(gs.game).c_str());
        else if (gs.foreground.empty() || gs.foreground == "fitsuite.exe") ui::muted("Ready. Off while FitSuite is in front.");
        else ui::muted("Ready. Off (%s is in front).", game_display_name(gs.foreground).c_str());
    } else if (everywhere) {
        ImGui::TextColored(theme::v4(theme::kGood), "On everywhere (Raw Accel default)");
    } else {
        ui::muted("Off");
    }
    if (fg.enabled && !gs.error.empty()) ui::callout(ui::Tone::Warn, "Switching is paused", gs.error.c_str());
    if (fg.enabled && gs.error.empty() && gs.writer_slow)
        ui::callout(ui::Tone::Warn, "Raw Accel's writer is still open", "It may be showing a message: look for its window.");
    if (!ui_.patch_message.empty()) ui::wrapped(ui_.patch_message.c_str(), theme::kTextMuted);
    for (const auto& wmsg : ui_.patch_warnings) ui::callout(ui::Tone::Warn, nullptr, wmsg.c_str());

    // The other way: the fix as the Raw Accel default, on everywhere and all the time.
    if (!fg.enabled && driver) {
        ImGui::Dummy(ImVec2(0, 2));
        ImGui::Separator();
        if (everywhere) {
            ui::faint("On everywhere since %s. FitSuite does not need to run for that.", short_time(everywhere->applied_utc).c_str());
            ImGui::BeginDisabled(app.rawaccel_busy());
            if (ui::button_secondary("Undo (restore my previous Raw Accel settings)")) {
                std::string err;
                if (!app.rawaccel_restore(*const_cast<AppliedChange*>(everywhere), &err)) ui_.patch_message = err;
            }
            ImGui::EndDisabled();
        } else {
            ImGui::BeginDisabled(!chosen || app.rawaccel_busy());
            if (ui::button_secondary("Use it everywhere instead…")) ui_.confirm_everywhere = !ui_.confirm_everywhere;
            ImGui::EndDisabled();
            ImGui::SameLine();
            ui::faint("Always on, in every program; FitSuite does not need to run.");
            if (ui_.confirm_everywhere && chosen) {
                ui::callout(ui::Tone::Neutral, "Use the fix everywhere?",
                            "FitSuite saves a copy of your Raw Accel settings.json, then makes the fix your Raw Accel default: on "
                            "for all mouse movement, in every program, also after a restart. You can undo it here.");
                if (ui::button_primary("Use it everywhere")) {
                    use_in_rawaccel(app, fix->transform.raw_accel, false);
                    ui_.confirm_everywhere = false;
                }
                ImGui::SameLine();
                if (ui::button_secondary("Cancel")) ui_.confirm_everywhere = false;
            }
        }
    }
    ui::end_card();
}

}  // namespace

}  // namespace fitapp
