#include <algorithm>
#include <cmath>
#include <filesystem>

#include "app.hpp"
#include "fit/store.hpp"
#include "fit/timeutil.hpp"
#include "imgui.h"
#include "implot.h"

namespace fitapp {

using namespace fit;
using theme::fonts;
namespace fs = std::filesystem;

std::vector<ConfigRuns> frame_config_runs(const SetupProfile& s) {
    std::vector<ConfigRuns> out;
    const FrameConfig* base = frame_baseline(s);
    auto add = [&](const FrameConfig& c) {
        ConfigRuns r;
        r.id = c.id;
        r.label = c.label;
        for (const auto& run : c.runs) r.runs.push_back(run.metrics);
        out.push_back(r);
    };
    if (base) add(*base);
    for (const auto& c : s.frame_configs)
        if (&c != base) add(c);
    return out;
}

const FrameConfig* frame_baseline(const SetupProfile& s) {
    for (const auto& c : s.frame_configs)
        if (c.baseline) return &c;
    return s.frame_configs.empty() ? nullptr : &s.frame_configs.front();
}

namespace {

ImPlotSpec line_spec(unsigned c, float w = 1.5f) {
    ImPlotSpec s;
    s.LineColor = theme::v4(c);
    s.LineWeight = w;
    return s;
}

FrameConfig* find_config(SetupProfile& s, const std::string& id) {
    for (auto& c : s.frame_configs)
        if (c.id == id) return &c;
    return nullptr;
}

std::string setting_instruction(const std::string& key, const std::string& value) {
    if (key == "fps_cap")
        return value == "0" || value == "off" ? "VALORANT → Settings → Video → General → Limit FPS Always: Off"
                                              : "VALORANT → Settings → Video → General → Limit FPS Always: On, Max FPS Always: " + value;
    if (key == "graphics_quality") return "VALORANT → Settings → Video → Graphics Quality → set the quality options to " + value;
    if (key == "resolution") return "VALORANT → Settings → Video → General → Resolution: " + value;
    if (key == "display_mode") return "VALORANT → Settings → Video → General → Display Mode: " + value;
    if (key == "vsync") return "VALORANT → Settings → Video → Graphics Quality → VSync: " + value;
    if (key == "reflex") return "VALORANT → Settings → Video → Graphics Quality → NVIDIA Reflex Low Latency: " + value;
    return "Set \"" + key + "\" to " + value + " in VALORANT.";
}

const FrameCapture* capture_for(App& app, const FrameRun& run) {
    auto& cache = app.frame_ui.captures;
    auto it = cache.find(run.id);
    if (it != cache.end()) return &it->second;
    if (run.csv_file.empty()) return nullptr;
    std::string err;
    auto text = read_file(app.store().root() / run.csv_file, &err);
    if (!text) return nullptr;
    CsvParseOptions o;
    o.process_name = run.application;
    auto cap = parse_presentmon_csv(*text, o, &err);
    if (!cap) return nullptr;
    return &cache.emplace(run.id, std::move(*cap)).first->second;
}

void add_run_from_csv(App& app, FrameConfig& cfg, const fs::path& csv, const std::string& origin) {
    auto& setup = app.setup();
    std::string err;
    auto text = read_file(csv, &err);
    if (!text) {
        app.toast(ui::Tone::Bad, err);
        return;
    }
    CsvParseOptions o;
    o.process_name = "VALORANT-Win64-Shipping.exe";
    auto cap = parse_presentmon_csv(*text, o, &err);
    bool other_app = false;
    if (!cap) {
        o.process_name.clear();
        std::string err2;
        cap = parse_presentmon_csv(*text, o, &err2);
        other_app = cap.has_value();
        if (!cap) {
            app.toast(ui::Tone::Bad, "Could not read the capture: " + err2, 10);
            return;
        }
    }
    const FrameStats st = compute_frame_stats(*cap);
    FrameRun run;
    run.id = make_id("run");
    run.timestamp_utc = utc_now_iso();
    run.origin = origin;
    run.application = cap->application;
    run.source_format = cap->source_format;
    run.present_mode = cap->present_mode;
    run.duration_s = st.duration_s;
    run.metrics = run_metrics(st);
    run.ft_p50_ms = st.ft_p50_ms;
    run.ft_p95_ms = st.ft_p95_ms;
    run.low01_fps = st.low01_fps;
    run.dropped_pct = st.dropped_pct;
    run.cpu_busy_p50_ms = st.cpu_busy_p50_ms;
    run.gpu_busy_p50_ms = st.gpu_busy_p50_ms;
    run.bound_hint = st.bound_hint;
    run.conditions = conditions_for("framefit", setup);
    const fs::path dst = app.store().framefit_dir() / (run.id + ".csv");
    if (atomic_write_file(dst, *text, &err)) run.csv_file = "framefit/" + run.id + ".csv";
    app.frame_ui.captures.emplace(run.id, std::move(*cap));
    cfg.runs.push_back(run);
    app.frame_ui.plot_run = run.id;
    app.record_result("framefit", "frame-run",
                      ui::fmt("\"%s\": %.0f FPS average, %.0f FPS 1%% low, %.2f ms 99th percentile", cfg.label.c_str(),
                              run.metrics.avg_fps, run.metrics.low1_fps, run.metrics.p99_ms),
                      origin);
    app.save();
    if (other_app)
        app.toast(ui::Tone::Warn, "No VALORANT frames in that file; used \"" + run.application + "\" instead.", 8);
    else
        app.toast(ui::Tone::Good, ui::fmt("Run added to \"%s\".", cfg.label.c_str()));
}

// Shown wherever a capture can be started: progress, cancel, and the last failure.
void capture_status_ui(App& app) {
    auto& f = app.frame_ui;
    if (f.capturing) {
        ui::callout(ui::Tone::Info, f.capture_state == CaptureState::Recording ? "Recording…" : "Waiting…", f.capture_message.c_str());
        if (ui::button_secondary("Cancel recording")) {
            app.platform().presentmon().cancel();
            f.capturing = false;
        }
    }
    if (!f.last_error.empty()) ui::callout(ui::Tone::Bad, "Recording failed", f.last_error.c_str());
}

void source_card(App& app, float w) {
    auto& f = app.frame_ui;
    ui::begin_card("##ff_src", w, 0);
    ui::card_title("Measurement source: Intel PresentMon",
                   "PresentMon reads Windows' graphics events (ETW). It does not inject into the game.");
    presentmon_ready(app);
    const PresentMonStatus& st = f.pm_status;
    ui::kv("Status", st.available ? "Ready" : "Not found", st.available ? theme::kGood : theme::kWarn);
    if (!st.path.empty()) ui::kv("Program", st.path);
    if (!st.note.empty()) ui::wrapped(st.note.c_str(), theme::kTextMuted);
    if (!f.presentmon_path[0] && !st.path.empty()) std::snprintf(f.presentmon_path, sizeof f.presentmon_path, "%s", st.path.c_str());
    ImGui::SetNextItemWidth(-130);
    ImGui::InputTextWithHint("##pmpath", "Path to PresentMon-2.x-x64.exe", f.presentmon_path, sizeof f.presentmon_path);
    ImGui::SameLine();
    if (ui::button_secondary("Browse…")) {
        if (auto p = app.platform().open_file_dialog("Find PresentMon", "PresentMon console app", "PresentMon*.exe")) {
            std::snprintf(f.presentmon_path, sizeof f.presentmon_path, "%s", p->string().c_str());
            app.platform().presentmon().set_path(f.presentmon_path);
            f.pm_checked_at = 0.0;
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        app.platform().presentmon().set_path(f.presentmon_path);
        f.pm_checked_at = 0.0;
    }
    if (ui::button_link("Get PresentMon (github.com/GameTechDev/PresentMon, MIT licence)"))
        app.platform().open_url("https://github.com/GameTechDev/PresentMon/releases");
    ui::end_card();
}

void configs_card(App& app, float w) {
    auto& setup = app.setup();
    auto& f = app.frame_ui;
    ui::begin_card("##ff_cfg", w, 0);
    ui::card_title("Configurations", "Your baseline and the settings you want to compare. Change one thing at a time.");
    if (setup.frame_configs.empty()) {
        FrameConfig b;
        b.id = make_id("cfg");
        b.label = "Your current settings";
        b.baseline = true;
        if (setup.game.fps_cap > 0) b.settings["fps_cap"] = std::to_string(setup.game.fps_cap);
        setup.frame_configs.push_back(b);
        app.save();
    }
    if (f.selected_config.empty() || !find_config(setup, f.selected_config)) f.selected_config = setup.frame_configs.front().id;
    for (auto& c : setup.frame_configs) {
        ImGui::PushID(c.id.c_str());
        const bool sel = c.id == f.selected_config;
        const ImVec2 row = ImGui::GetCursorPos();
        const float row_h = 28.0f;
        if (ImGui::Selectable("##cfg", sel, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, row_h))) f.selected_config = c.id;
        const ImVec2 after = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(row.x + 8, row.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f));
        ImGui::TextUnformatted(c.label.c_str());
        if (c.baseline) {
            ImGui::SameLine(0, 10);
            ui::pill("BASELINE", theme::kAccent);
        }
        ImGui::SameLine(0, 12);
        ui::faint("%s", c.runs.size() == 1 ? "1 run" : ui::fmt("%zu runs", c.runs.size()).c_str());
        ImGui::SetCursorPos(after);
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::SetNextItemWidth(-110);
    ImGui::InputTextWithHint("##newcfg", "New configuration, e.g. \"FPS cap 237\"", f.new_label, sizeof f.new_label);
    ImGui::SameLine();
    ImGui::BeginDisabled(!f.new_label[0]);
    if (ui::button_secondary("Add")) {
        FrameConfig c;
        c.id = make_id("cfg");
        c.label = f.new_label;
        setup.frame_configs.push_back(c);
        f.selected_config = c.id;
        f.new_label[0] = 0;
        app.save();
    }
    ImGui::EndDisabled();
    ui::end_card();
}

void selected_card(App& app) {
    auto& setup = app.setup();
    auto& f = app.frame_ui;
    FrameConfig* cfg = find_config(setup, f.selected_config);
    if (!cfg) return;
    ui::begin_card("##ff_sel", 0, 0);
    ui::card_title(cfg->label.c_str(), cfg->baseline ? "Baseline: the configuration everything else is compared with."
                                                     : "Candidate configuration.");
    ui::push_font(fonts().semibold, theme::kSmall);
    ImGui::TextUnformatted("SETTINGS (CHANGED BY YOU IN THE GAME)");
    ui::pop_font();
    std::string remove;
    for (const auto& [k, v] : cfg->settings) {
        ImGui::PushID(k.c_str());
        ui::wrapped(("• " + setting_instruction(k, v)).c_str());
        ImGui::SameLine();
        if (ui::button_link("remove")) remove = k;
        ImGui::PopID();
    }
    if (!remove.empty()) {
        cfg->settings.erase(remove);
        cfg->user_confirmed = false;
        app.save();
    }
    if (cfg->settings.empty()) ui::faint("No settings listed. Add the ones that differ from your baseline.");
    static const char* keys[] = {"fps_cap", "graphics_quality", "resolution", "display_mode", "vsync", "reflex", "other"};
    static int key_i = 0;
    ImGui::TextUnformatted("Setting:");
    for (int i = 0; i < IM_ARRAYSIZE(keys); ++i) {
        ImGui::SameLine();
        ImGui::RadioButton(keys[i], &key_i, i);
    }
    if (key_i == 6) {
        ImGui::SetNextItemWidth(150);
        ImGui::InputTextWithHint("##k", "setting name", f.new_key, sizeof f.new_key);
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(150);
    ImGui::InputTextWithHint("##v", "value", f.new_value, sizeof f.new_value);
    ImGui::SameLine();
    if (ui::button_secondary("Add setting") && f.new_value[0]) {
        const std::string k = key_i == 6 ? std::string(f.new_key) : keys[key_i];
        if (!k.empty()) {
            cfg->settings[k] = f.new_value;
            cfg->user_confirmed = false;
            f.new_value[0] = 0;
            app.save();
        }
    }
    bool conf = cfg->user_confirmed;
    if (ImGui::Checkbox("I have set these in VALORANT", &conf)) {
        cfg->user_confirmed = conf;
        app.save();
    }
    ui::help_marker("VALORANT has no supported settings interface, so FitSuite never changes game settings. This checkbox "
                    "records that you changed them; runs are labelled with it.");
    ui::end_card();

    // ---- record ----
    ui::begin_card("##ff_rec", 0, 0);
    ui::card_title("Record a run", "Use the same scenario every time: same map spot or route, same duration, nothing else "
                                   "running. The Range is the most repeatable place.");
    CaptureRequest& rq = f.request;
    static const int durations[] = {30, 60, 90, 120};
    int di = rq.duration_s >= 120 ? 3 : rq.duration_s >= 90 ? 2 : rq.duration_s >= 60 ? 1 : 0;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Duration:");
    static const char* dur_labels[] = {"30 s", "60 s", "90 s", "120 s"};
    for (int i = 0; i < 4; ++i) {
        ImGui::SameLine();
        if (ImGui::RadioButton(dur_labels[i], di == i)) rq.duration_s = durations[i];
    }
    static int start_mode = 0;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Start:");
    ImGui::SameLine();
    ImGui::RadioButton("Press F10 in game", &start_mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("After a 10 s countdown", &start_mode, 1);
    rq.hotkey = start_mode == 0 ? "F10" : "";
    rq.delay_s = start_mode == 1 ? 10 : 0;
    ImGui::Checkbox("Run PresentMon as administrator", &rq.elevate);
    ui::help_marker("PresentMon needs administrator rights unless your account is in the \"Performance Log Users\" "
                    "group. FitSuite itself never needs administrator rights.");
    const bool can = presentmon_ready(app) && !f.capturing;
    if (!cfg->user_confirmed && !cfg->settings.empty())
        ui::callout(ui::Tone::Warn, "Settings not confirmed", "Tick \"I have set these in VALORANT\" once the game uses them.");
    ImGui::BeginDisabled(!can);
    if (ui::button_primary("Start capture")) framefit_start_capture(app, cfg->id);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ui::button_secondary("Import PresentMon CSV…")) {
        if (auto p = app.platform().open_file_dialog("Import a PresentMon capture", "PresentMon CSV", "*.csv"))
            add_run_from_csv(app, *cfg, *p, "measured");
    }
    capture_status_ui(app);
    ui::end_card();

    // ---- runs ----
    if (cfg->runs.empty()) return;
    ui::begin_card("##ff_runs", 0, 0);
    ui::card_title("Runs", "Each row is one measured run. Values are from PresentMon's frame-by-frame data.");
    std::string del;
    if (ImGui::BeginTable("##runs", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        for (const char* h : {"When", "Length", "Avg FPS", "1% low FPS", "P99 frame time", "Stutters/min", "Latency"})
            ImGui::TableSetupColumn(h);
        ImGui::TableSetupColumn("Data", ImGuiTableColumnFlags_WidthFixed, 132.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableHeadersRow();
        for (const auto& r : cfg->runs) {
            ImGui::PushID(r.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(short_time(r.timestamp_utc).c_str(), f.plot_run == r.id, ImGuiSelectableFlags_SpanAllColumns))
                f.plot_run = r.id;
            ui::push_font(fonts().mono, theme::kMono);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f s", r.duration_s);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", r.metrics.avg_fps);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", r.metrics.low1_fps);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f ms", r.metrics.p99_ms);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", r.metrics.stutters_per_min);
            ImGui::TableNextColumn();
            if (std::isfinite(r.metrics.latency_p50_ms)) ImGui::Text("%.1f ms", r.metrics.latency_p50_ms);
            else ImGui::TextDisabled("n/a");
            if (std::isfinite(r.metrics.latency_p50_ms) && ImGui::IsItemHovered())
                ImGui::SetTooltip("Median PresentMon latency, %s. Not mouse-to-photon latency.", r.metrics.latency_definition.c_str());
            ui::pop_font();
            ImGui::TableNextColumn();
            ui::origin_badge(origin_of(r.origin));
            ImGui::TableNextColumn();
            if (ui::confirm_link("delete", "Delete this run?")) del = r.id;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!del.empty()) {
        cfg->runs.erase(std::remove_if(cfg->runs.begin(), cfg->runs.end(), [&](const FrameRun& r) { return r.id == del; }), cfg->runs.end());
        app.save();
    }
    if (cfg->runs.size() >= 2) {
        std::vector<double> a, l;
        for (const auto& r : cfg->runs) {
            a.push_back(r.metrics.avg_fps);
            l.push_back(r.metrics.low1_fps);
        }
        const double cva = stddev(a) / mean(a) * 100, cvl = stddev(l) / mean(l) * 100;
        ui::muted("Run-to-run variation: average FPS ±%.1f%%, 1%% low ±%.1f%% (coefficient of variation).", cva, cvl);
        if (cva > 3.0 || cvl > 8.0)
            ui::callout(ui::Tone::Warn, "These runs differ a lot",
                        "Comparisons with this configuration will be flagged as noisy. Repeat the same scenario more carefully.");
    }
    // Plot the selected run.
    const FrameRun* pr = nullptr;
    for (const auto& r : cfg->runs)
        if (r.id == f.plot_run) pr = &r;
    if (!pr) pr = &cfg->runs.back();
    if (const FrameCapture* cap = capture_for(app, *pr)) {
        std::vector<double> t, ft;
        double acc = 0;
        for (const auto& fr : cap->frames) {
            acc += fr.interval_ms;
            t.push_back(acc / 1000.0);
            ft.push_back(fr.interval_ms);
        }
        const FrameStats st = compute_frame_stats(*cap);
        std::vector<double> sx, sy;
        for (size_t i : st.stutter_frames) {
            sx.push_back(t[i]);
            sy.push_back(ft[i]);
        }
        const float w = ImGui::GetContentRegionAvail().x;
        if (ImPlot::BeginPlot("Frame time over the run", ImVec2(w * 0.62f, 250), ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
            ImPlot::SetupAxes("Time (s)", "Frame time (ms)", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Outside | ImPlotLegendFlags_Horizontal);
            ImPlot::PlotLine("Frame time", t.data(), ft.data(), int(t.size()), line_spec(theme::kAccent, 1.0f));
            if (!sx.empty()) {
                ImPlotSpec ms;
                ms.Marker = ImPlotMarker_Circle;
                ms.MarkerSize = 3.5f;
                ms.MarkerFillColor = theme::v4(theme::kWarn);
                ms.MarkerLineColor = theme::v4(theme::kWarn);
                ImPlot::PlotScatter("Stutter", sx.data(), sy.data(), int(sx.size()), ms);
            }
            ImPlot::EndPlot();
        }
        ImGui::SameLine();
        std::vector<double> sorted = ft, pct;
        std::sort(sorted.begin(), sorted.end());
        for (size_t i = 0; i < sorted.size(); ++i) pct.push_back(100.0 * double(i) / double(sorted.size() - 1));
        if (ImPlot::BeginPlot("Frame-time distribution", ImVec2(-1, 250), ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoLegend)) {
            ImPlot::SetupAxes("Percentile of frames", "Frame time (ms)", 0, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisLimits(ImAxis_X1, 50, 100, ImPlotCond_Once);
            ImPlot::PlotLine("Frame time", pct.data(), sorted.data(), int(pct.size()), line_spec(theme::kInfo, 2.0f));
            ImPlot::EndPlot();
        }
        ui::faint("%s · %s · present mode: %s%s", cap->application.c_str(), cap->source_format.c_str(),
                  cap->present_mode.empty() ? "unknown" : cap->present_mode.c_str(),
                  st.bound_hint == "gpu" ? " · GPU busy for most of each frame (GPU-limited)"
                  : st.bound_hint == "cpu" ? " · CPU busy for most of each frame (CPU-limited)"
                  : st.bound_hint == "cap-or-sync" ? " · neither CPU nor GPU fully busy (frame cap or sync)" : "");
    }
    ui::end_card();
}

void compare_card(App& app) {
    auto& setup = app.setup();
    auto& f = app.frame_ui;
    if (setup.frame_configs.size() < 2) return;
    ui::section("Compare configurations");
    ui::begin_card("##ff_cmp", 0, 0);
    if (f.compare_a.empty() || !find_config(setup, f.compare_a)) f.compare_a = frame_baseline(setup)->id;
    if (f.compare_b.empty() || !find_config(setup, f.compare_b) || f.compare_b == f.compare_a)
        for (const auto& c : setup.frame_configs)
            if (c.id != f.compare_a) { f.compare_b = c.id; break; }
    // Two plain lists side by side: the reference and the configuration compared with it.
    auto pick = [&](const char* title, const char* id, std::string& v, float w) {
        ImGui::BeginGroup();
        ImGui::TextUnformatted(title);
        const float row_h = ImGui::GetTextLineHeightWithSpacing();
        if (ImGui::BeginListBox(id, ImVec2(w, row_h * float(std::min<size_t>(setup.frame_configs.size(), 5)) + 6))) {
            for (const auto& x : setup.frame_configs)
                if (ImGui::Selectable((x.label + "##" + x.id).c_str(), x.id == v)) v = x.id;
            ImGui::EndListBox();
        }
        ImGui::EndGroup();
    };
    pick("Reference", "##cmp_a", f.compare_a, 300);
    ImGui::SameLine(0, 20);
    pick("Compared with it", "##cmp_b", f.compare_b, 300);
    FrameConfig* A = find_config(setup, f.compare_a);
    FrameConfig* B = find_config(setup, f.compare_b);
    if (!A || !B) {
        ui::end_card();
        return;
    }
    auto runs = [](const FrameConfig& c) {
        ConfigRuns r;
        r.id = c.id;
        r.label = c.label;
        for (const auto& x : c.runs) r.runs.push_back(x.metrics);
        return r;
    };
    const ConfigComparison cmp = compare_configs(runs(*A), runs(*B));
    ui::muted("%s", cmp.summary.c_str());
    for (const auto& n : cmp.notes) ui::callout(ui::Tone::Warn, nullptr, n.c_str());
    if (ImGui::BeginTable("##cmp", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        for (const char* h : {"Metric", "Reference", "Candidate", "Difference", "95% range of difference", "Verdict"})
            ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (const auto& m : cmp.metrics) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(m.label.c_str());
            ui::push_font(fonts().mono, theme::kMono);
            ImGui::TableNextColumn();
            ImGui::Text("%s  (n=%d)", ui::num(m.a_mean, 2).c_str(), m.n_a);
            ImGui::TableNextColumn();
            ImGui::Text("%s  (n=%d)", ui::num(m.b_mean, 2).c_str(), m.n_b);
            ImGui::TableNextColumn();
            ImGui::Text("%s (%s%%)", ui::signed_num(m.diff, 2).c_str(), ui::signed_num(m.diff_pct, 1).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s … %s", ui::signed_num(m.ci.lo, 2).c_str(), ui::signed_num(m.ci.hi, 2).c_str());
            ui::pop_font();
            ImGui::TableNextColumn();
            const unsigned col = m.outcome == MetricOutcome::Better ? theme::kGood
                                 : m.outcome == MetricOutcome::Worse ? theme::kBad : theme::kTextMuted;
            ui::pill(m.outcome == MetricOutcome::Better ? "BETTER" : m.outcome == MetricOutcome::Worse ? "WORSE"
                     : m.outcome == MetricOutcome::NoDifference ? "WITHIN NOISE" : "NEED MORE RUNS", col);
        }
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, 6));
    ui::push_font(fonts().semibold, theme::kBody);
    ImGui::TextUnformatted("Recommendation");
    ui::pop_font();
    ImGui::TextUnformatted("Goal:");
    ImGui::RadioButton("Consistency (lowest 99th-percentile frame time)", &f.goal, 0);
    ImGui::RadioButton("Throughput (highest average FPS)", &f.goal, 1);
    ImGui::RadioButton("Latency (lowest measured latency)", &f.goal, 2);
    const auto all = frame_config_runs(setup);
    const auto rec = recommend_config(all, FrameGoal(f.goal));
    ui::callout(rec.reliable ? ui::Tone::Good : ui::Tone::Neutral, rec.reliable ? ("Use: " + all[size_t(rec.best)].label).c_str() : "Keep the baseline",
                rec.reason.c_str());
    ui::faint("Trade-offs: an FPS cap a little below what your PC sustains usually lowers frame-time spikes and can reduce "
              "latency when the GPU is the limit, at the cost of average FPS. Higher average FPS shortens frame time but "
              "is not the same as lower mouse-to-screen latency, which also includes the mouse, USB, game and monitor.");
    ui::end_card();
}

}  // namespace

bool presentmon_ready(App& app) {
    auto& f = app.frame_ui;
    const double now = app.time();
    if (now - f.pm_checked_at > 2.0 || f.pm_checked_at <= 0.0) {  // looking for the program touches the disk
        f.pm_status = app.platform().presentmon().status();
        f.pm_checked_at = now;
    }
    return f.pm_status.available;
}

bool framefit_start_capture(App& app, const std::string& config_id) {
    auto& f = app.frame_ui;
    CaptureRequest& rq = f.request;
    rq.output_csv = app.store().framefit_dir() / ("capture-" + utc_compact_now() + ".csv");
    std::string err;
    if (!app.platform().presentmon().start(rq, &err)) {
        f.last_error = err;
        return false;
    }
    f.capturing = true;
    f.capture_config = config_id;
    f.capture_state = CaptureState::Waiting;
    f.capture_message = rq.hotkey.empty() ? "Recording starts after the countdown. Switch to VALORANT now."
                                          : "Switch to VALORANT and press " + rq.hotkey + " to start.";
    f.last_error.clear();
    return true;
}

// Runs every frame (from App::frame), so a recording finishes and is imported whatever page is open.
void framefit_tick(App& app) {
    auto& f = app.frame_ui;
    if (!f.capturing) return;
    const CaptureStatus st = app.platform().presentmon().poll();
    if (st.state == CaptureState::Finished) {
        f.capturing = false;
        if (FrameConfig* target = find_config(app.setup(), f.capture_config)) add_run_from_csv(app, *target, st.csv, "measured");
    } else if (st.state == CaptureState::Failed) {
        f.capturing = false;
        f.last_error = st.message;
    } else if (st.state == CaptureState::Idle) {
        f.capturing = false;
    } else {
        f.capture_state = st.state;
        f.capture_message = st.message;
    }
}

void framefit_import(App& app, const std::string& config_id, const fs::path& csv, const std::string& origin) {
    if (FrameConfig* c = find_config(app.setup(), config_id)) add_run_from_csv(app, *c, csv, origin);
}

// Full FrameFit view with every run, chart and comparison (Nerd stats → FrameFit).
void framefit_details(App& app) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float w = (avail - 14) * 0.5f;
    source_card(app, w);
    ImGui::SameLine(0, 14);
    configs_card(app, w);
    selected_card(app);
    compare_card(app);
    ui::begin_card("##ff_help", 0, 0);
    ui::card_title("What the numbers mean");
    {
        ui::wrapped("Frame time: the time between two consecutive frames presented by the game (PresentMon MsBetweenPresents, or "
                    "FrameTime in 2.x captures), in milliseconds.\n"
                    "Average FPS: frames divided by the total time of the run (not the average of per-frame FPS).\n"
                    "1% / 0.1% low FPS: 1000 divided by the mean of the slowest 1% / 0.1% of frame times.\n"
                    "99th percentile frame time: 99% of frames were faster than this (linear interpolation between frames).\n"
                    "Stutter: a frame at least twice as long as the median of the previous half second and at least 4 ms longer.\n"
                    "Latency: only when PresentMon reports it — either frame start to display (2.x DisplayLatency) or present to "
                    "display (MsUntilDisplayed). Neither includes the mouse, USB polling, or the monitor's pixel response, so "
                    "neither is end-to-end input latency.\n"
                    "Differences are only called better or worse when they are larger than run-to-run variation (95% Welch "
                    "interval) and above a practical threshold (3% average FPS, 5% for lows and percentiles).",
                    theme::kTextMuted);
    }
    ui::end_card();
}

// ---- simple view ---------------------------------------------------------------------------------
namespace {

struct RunAverage {
    int n = 0;
    double fps = kNaN, low = kNaN, p99 = kNaN, stutters = kNaN;
};

RunAverage average_runs(const FrameConfig& c) {
    RunAverage a;
    std::vector<double> f, l, p, st;
    for (const auto& r : c.runs) {
        if (std::isfinite(r.metrics.avg_fps)) f.push_back(r.metrics.avg_fps);
        if (std::isfinite(r.metrics.low1_fps)) l.push_back(r.metrics.low1_fps);
        if (std::isfinite(r.metrics.p99_ms)) p.push_back(r.metrics.p99_ms);
        if (std::isfinite(r.metrics.stutters_per_min)) st.push_back(r.metrics.stutters_per_min);
    }
    a.n = int(c.runs.size());
    if (!f.empty()) a.fps = mean(f);
    if (!l.empty()) a.low = mean(l);
    if (!p.empty()) a.p99 = mean(p);
    if (!st.empty()) a.stutters = mean(st);
    return a;
}

// Plain description of frame pacing: how far the slowest frames are from a typical frame.
const char* smoothness(const RunAverage& a) {
    if (!std::isfinite(a.p99) || !std::isfinite(a.fps) || a.fps <= 0) return "—";
    double grade = a.p99 / (1000.0 / a.fps);
    if (std::isfinite(a.stutters) && a.stutters > 10) grade += 0.5;
    return grade <= 1.5 ? "Very smooth" : grade <= 2.0 ? "Smooth" : grade <= 3.0 ? "Some hitches" : "Choppy";
}

ConfigRuns runs_of(const FrameConfig& c) {
    ConfigRuns r;
    r.id = c.id;
    r.label = c.label;
    for (const auto& x : c.runs) r.runs.push_back(x.metrics);
    return r;
}

}  // namespace

void page_framefit(App& app) {
    auto& setup = app.setup();
    auto& f = app.frame_ui;
    ui::page_header("FrameFit", "Checks how smoothly VALORANT runs on your PC, and whether a settings change really helps. "
                                "FitSuite never changes your game or system settings.");
    if (setup.frame_configs.empty()) {
        FrameConfig b;
        b.id = make_id("cfg");
        b.label = "Your current settings";
        b.baseline = true;
        setup.frame_configs.push_back(b);
        app.save();
    }
    const bool ready = presentmon_ready(app);
    if (!ready) {
        ui::begin_card("##ffs_pm", 0, 0);
        ui::card_title("PresentMon is needed",
                       "FrameFit reads frame timing with PresentMon, a free tool from Intel. It does not touch the game.");
        if (ui::button_primary("Download PresentMon")) app.platform().open_url("https://github.com/GameTechDev/PresentMon/releases");
        ImGui::SameLine();
        if (ui::button_secondary("Find it…")) {
            if (auto p = app.platform().open_file_dialog("Find PresentMon", "PresentMon console app", "PresentMon*.exe")) {
                app.platform().presentmon().set_path(p->string());
                f.pm_checked_at = 0.0;
            }
        }
        ImGui::SameLine();
        ui::faint("Get PresentMon-2.x.x-x64.exe and put it in a \"tools\" folder next to FitSuite.exe.");
        ui::end_card();
    }
    capture_status_ui(app);

    FrameConfig* base = const_cast<FrameConfig*>(frame_baseline(setup));
    ui::begin_card("##ffs_runs", 0, 0);
    ui::card_title("Settings you are comparing",
                   "Record three 60-second runs for each, doing the same thing in the same place (the Range is ideal). Press "
                   "F10 in VALORANT to start each run.");
    bool sim = false, any_runs = false;
    for (const auto& c : setup.frame_configs)
        for (const auto& r : c.runs) {
            sim = sim || r.origin == "simulated";
            any_runs = true;
        }
    std::string record_for;
    const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##ffs_table", 7, tf)) {
        ImGui::TableSetupColumn("Settings", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Runs", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Average FPS", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("1% low FPS", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Smoothness", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Compared with yours", ImGuiTableColumnFlags_WidthFixed, 170);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableHeadersRow();
        for (auto& c : setup.frame_configs) {
            ImGui::PushID(c.id.c_str());
            const RunAverage a = average_runs(c);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(c.label.c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(theme::v4(a.n >= 3 ? theme::kText : theme::kTextMuted), "%d of 3", a.n);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(std::isfinite(a.fps) ? ui::num(a.fps, 0).c_str() : "—");
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(std::isfinite(a.low) ? ui::num(a.low, 0).c_str() : "—");
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(smoothness(a));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (&c == base) {
                ui::muted("(yours)");
            } else {
                const ConfigComparison cmp = compare_configs(runs_of(*base), runs_of(c));
                int better = 0, worse = 0;
                for (const auto& m : cmp.metrics) {
                    better += m.outcome == MetricOutcome::Better ? 1 : 0;
                    worse += m.outcome == MetricOutcome::Worse ? 1 : 0;
                }
                if (cmp.preliminary) ui::muted("needs 3 runs each");
                else if (cmp.noisy) ImGui::TextColored(theme::v4(theme::kWarn), "runs too different");
                else if (better && !worse) ImGui::TextColored(theme::v4(theme::kGood), "better");
                else if (worse && !better) ImGui::TextColored(theme::v4(theme::kBad), "worse");
                else if (better && worse) ImGui::TextColored(theme::v4(theme::kWarn), "trade-off");
                else ui::muted("no real difference");
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!ready || f.capturing);
            if (ui::button_secondary("Record a run", ImVec2(-1, 0))) record_for = c.id;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!record_for.empty()) framefit_start_capture(app, record_for);
    if (any_runs) {
        ui::faint("Numbers:");
        ImGui::SameLine();
        ui::origin_badge(sim ? ui::Origin::Simulated : ui::Origin::Measured);
    }
    ImGui::SetNextItemWidth(320);
    ImGui::InputTextWithHint("##ffs_new", "What did you change? e.g. Max FPS 237", f.simple_new_label, sizeof f.simple_new_label);
    ImGui::SameLine();
    ImGui::BeginDisabled(!f.simple_new_label[0]);
    if (ui::button_secondary("Add")) {
        FrameConfig c;
        c.id = make_id("cfg");
        c.label = f.simple_new_label;
        c.user_confirmed = true;  // added after the player changed it in the game
        setup.frame_configs.push_back(c);
        f.simple_new_label[0] = 0;
        app.save();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, 20);
    ImGui::Checkbox("Run PresentMon as administrator", &f.request.elevate);
    ui::help_marker("PresentMon needs administrator rights unless your account is in the \"Performance Log Users\" group. "
                    "Windows will ask for permission. FitSuite itself never needs administrator rights.");
    ui::end_card();

    const auto all = frame_config_runs(setup);
    if (all.size() >= 2) {
        const auto rec = recommend_config(all, FrameGoal::Consistency);
        bool any_ready = false;
        for (size_t i = 1; i < all.size(); ++i) any_ready = any_ready || (all[i].runs.size() >= 3 && all[0].runs.size() >= 3);
        if (any_ready)
            ui::callout(rec.reliable ? ui::Tone::Good : ui::Tone::Neutral,
                        rec.reliable ? ("Use: " + all[size_t(rec.best)].label).c_str() : "Keep your current settings",
                        rec.reliable ? rec.reason.c_str() : "Nothing you tried was reliably smoother than your current settings.");
    }
    ImGui::Dummy(ImVec2(0, 2));
    if (ui::button_link("All the numbers: Nerd stats → Frame rate")) {
        app.nerd_tab = 1;
        app.go(Page::Nerd);
    }
}

}  // namespace fitapp
