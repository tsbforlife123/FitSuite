#include "fit/frame.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace fit {

namespace {

std::string lower(std::string_view s) {
    std::string o(s);
    for (auto& c : o) c = char(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '"')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '"')) s.remove_suffix(1);
    return s;
}

void split(std::string_view line, std::vector<std::string_view>& out) {
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            out.push_back(trim(line.substr(start, i - start)));
            start = i + 1;
        }
    }
}

double to_num(std::string_view s) {
    if (s.empty()) return kNaN;
    if (s == "NA" || s == "na" || s == "N/A") return kNaN;
    std::string tmp(s);
    char* end = nullptr;
    const double v = std::strtod(tmp.c_str(), &end);
    if (end == tmp.c_str()) return kNaN;
    return v;
}

struct ColumnMap {
    std::map<std::string, int> idx;  // lower-case header -> column
    int find(std::initializer_list<const char*> names, std::string* used, std::vector<std::string_view>* header) const {
        for (const char* n : names) {
            auto it = idx.find(n);
            if (it != idx.end()) {
                if (used && header) *used = std::string((*header)[size_t(it->second)]);
                return it->second;
            }
        }
        return -1;
    }
};

struct RowGroup {
    std::string app;
    uint32_t pid = 0;
    std::string swap_chain;
    std::vector<size_t> rows;
};

}  // namespace

std::optional<FrameCapture> parse_presentmon_csv(std::string_view text, const CsvParseOptions& opt, std::string* error) {
    auto set_err = [&](const std::string& e) {
        if (error) *error = e;
        return std::nullopt;
    };
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.remove_prefix(3);

    std::vector<std::string_view> lines;
    {
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n') {
                std::string_view l = text.substr(start, i - start);
                if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
                if (!trim(l).empty()) lines.push_back(l);
                start = i + 1;
            }
        }
    }
    if (lines.size() < 2) return set_err("The file has no frame rows.");

    std::vector<std::string_view> header;
    split(lines[0], header);
    ColumnMap cm;
    for (size_t i = 0; i < header.size(); ++i) cm.idx.emplace(lower(header[i]), int(i));

    FrameCapture cap;
    std::string used;
    const int c_app = cm.find({"application"}, nullptr, nullptr);
    const int c_pid = cm.find({"processid"}, nullptr, nullptr);
    const int c_sc = cm.find({"swapchainaddress"}, nullptr, nullptr);
    const int c_interval = cm.find({"msbetweenpresents", "frametime", "msbetweenappstart"}, &used, &header);
    if (c_interval < 0)
        return set_err("This does not look like a PresentMon CSV: no frame-interval column (MsBetweenPresents or FrameTime).");
    cap.column_used["frame_interval"] = used;
    auto opt_col = [&](const char* metric, std::initializer_list<const char*> names) {
        std::string u;
        const int c = cm.find(names, &u, &header);
        if (c >= 0) cap.column_used[metric] = u;
        return c;
    };
    const int c_disp = opt_col("displayed_interval", {"msbetweendisplaychange", "displayedtime"});
    const int c_dropped = opt_col("dropped", {"dropped"});
    const int c_cpub = opt_col("cpu_busy", {"cpubusy", "mscpubusy"});
    const int c_cpuw = opt_col("cpu_wait", {"cpuwait", "mscpuwait"});
    const int c_gpub = opt_col("gpu_busy", {"gpubusy", "msgpubusy", "msgpuactive"});
    const int c_gput = opt_col("gpu_time", {"gputime", "msgputime"});
    const int c_gpul = opt_col("gpu_latency", {"gpulatency", "msgpulatency", "msuntilrenderstart"});
    const int c_dlat = opt_col("display_latency", {"displaylatency"});
    const int c_until = opt_col("until_displayed", {"msuntildisplayed"});
    const int c_pcl = opt_col("pc_latency", {"mspclatency", "pclatency"});
    const int c_ctp = opt_col("click_to_photon", {"clicktophotonlatency", "msclicktophotonlatency"});
    const int c_aip = opt_col("all_input_to_photon", {"allinputtophotonlatency", "msallinputtophotonlatency"});
    const int c_rt = cm.find({"runtime", "presentruntime"}, nullptr, nullptr);
    const int c_mode = cm.find({"presentmode"}, nullptr, nullptr);
    const int c_sync = cm.find({"syncinterval"}, nullptr, nullptr);
    const int c_tear = cm.find({"allowstearing"}, nullptr, nullptr);

    const bool has_v2 = cm.idx.count("frametime") && cm.idx.count("cpubusy");
    const bool has_new = cm.idx.count("msbetweenpresents") && cm.idx.count("mscpubusy");
    const bool has_v1 = cm.idx.count("msbetweenpresents") && cm.idx.count("dropped");
    cap.source_format = has_new ? "PresentMon 2.4+ (default metrics)"
                        : has_v2 ? "PresentMon 2.x metrics"
                        : has_v1 ? "PresentMon 1.x metrics"
                                 : "PresentMon-compatible CSV";
    if (cap.column_used["frame_interval"] == "FrameTime" || lower(cap.column_used["frame_interval"]) == "frametime")
        cap.warnings.push_back("Frame interval read from 2.x 'FrameTime' (CPU start to CPU start of the next frame).");

    // Group rows by process and swap chain.
    std::vector<std::vector<std::string_view>> rows;
    rows.reserve(lines.size() - 1);
    std::map<std::string, RowGroup> groups;
    std::vector<std::string_view> f;
    size_t bad = 0;
    for (size_t li = 1; li < lines.size(); ++li) {
        split(lines[li], f);
        if (f.size() < header.size()) { ++bad; continue; }
        // PresentMon repeats nothing mid-file, but merged files may repeat the header.
        if (lower(f[size_t(c_interval)]) == lower(header[size_t(c_interval)])) continue;
        const std::string app = c_app >= 0 ? std::string(f[size_t(c_app)]) : std::string("<unknown>");
        const uint32_t pid = c_pid >= 0 ? uint32_t(std::strtoul(std::string(f[size_t(c_pid)]).c_str(), nullptr, 10)) : 0;
        const std::string sc = c_sc >= 0 ? std::string(f[size_t(c_sc)]) : std::string();
        const std::string key = app + "|" + std::to_string(pid) + "|" + sc;
        auto& g = groups[key];
        g.app = app;
        g.pid = pid;
        g.swap_chain = sc;
        g.rows.push_back(rows.size());
        rows.push_back(f);
    }
    if (groups.empty()) return set_err("The file has no readable frame rows.");

    const RowGroup* pick = nullptr;
    std::set<std::string> apps;
    for (const auto& kv : groups) apps.insert(kv.second.app);
    const std::string want = lower(opt.process_name);
    for (const auto& kv : groups) {
        if (!want.empty() && lower(kv.second.app) != want) continue;
        if (!pick || kv.second.rows.size() > pick->rows.size()) pick = &kv.second;
    }
    if (!pick) {
        std::string list;
        for (const auto& a : apps) list += (list.empty() ? "" : ", ") + a;
        return set_err("No frames from " + opt.process_name + " in this file (found: " + list + ").");
    }
    for (const auto& a : apps)
        if (a != pick->app) cap.other_applications.push_back(a);
    cap.application = pick->app;
    cap.process_id = pick->pid;
    cap.swap_chain = pick->swap_chain;
    size_t other_chains = 0;
    for (const auto& kv : groups)
        if (kv.second.app == pick->app && &kv.second != pick) ++other_chains;
    if (other_chains)
        cap.warnings.push_back("The process presented to more than one swap chain; using the one with the most frames.");

    std::map<std::string, size_t> modes;
    auto num = [&](const std::vector<std::string_view>& r, int c) { return c >= 0 ? to_num(r[size_t(c)]) : kNaN; };
    for (size_t ri : pick->rows) {
        const auto& r = rows[ri];
        FrameRecord fr;
        fr.interval_ms = num(r, c_interval);
        if (!(fr.interval_ms > 0.0) || fr.interval_ms > 10000.0 || !std::isfinite(fr.interval_ms)) { ++bad; continue; }
        fr.displayed_interval_ms = num(r, c_disp);
        if (c_dropped >= 0) fr.displayed = num(r, c_dropped) > 0.5 ? 0 : 1;
        else if (c_disp >= 0 && cap.column_used["displayed_interval"] == header[size_t(c_disp)] &&
                 lower(header[size_t(c_disp)]) == "displayedtime")
            fr.displayed = std::isfinite(fr.displayed_interval_ms) ? 1 : 0;
        fr.cpu_busy_ms = num(r, c_cpub);
        fr.cpu_wait_ms = num(r, c_cpuw);
        fr.gpu_busy_ms = num(r, c_gpub);
        fr.gpu_time_ms = num(r, c_gput);
        fr.gpu_latency_ms = num(r, c_gpul);
        fr.display_latency_ms = num(r, c_dlat);
        fr.until_displayed_ms = num(r, c_until);
        fr.pc_latency_ms = num(r, c_pcl);
        fr.click_to_photon_ms = num(r, c_ctp);
        fr.all_input_to_photon_ms = num(r, c_aip);
        cap.frames.push_back(fr);
        if (c_mode >= 0) ++modes[std::string(r[size_t(c_mode)])];
        if (cap.runtime.empty() && c_rt >= 0) cap.runtime = std::string(r[size_t(c_rt)]);
        if (cap.sync_interval < 0 && c_sync >= 0) cap.sync_interval = int(num(r, c_sync));
        if (cap.allows_tearing < 0 && c_tear >= 0) cap.allows_tearing = int(num(r, c_tear));
    }
    size_t best = 0;
    for (const auto& kv : modes)
        if (kv.second > best) { best = kv.second; cap.present_mode = kv.first; }
    if (bad) cap.warnings.push_back(std::to_string(bad) + " unreadable rows were skipped.");
    if (cap.frames.size() < 2) return set_err("Too few valid frames for " + cap.application + ".");
    return cap;
}

// ---- statistics ----------------------------------------------------------------------------

namespace {

double mean_of_slowest(const std::vector<double>& sorted_desc, double fraction) {
    if (sorted_desc.empty()) return kNaN;
    const size_t k = std::max<size_t>(1, size_t(std::floor(double(sorted_desc.size()) * fraction)));
    double s = 0.0;
    for (size_t i = 0; i < k; ++i) s += sorted_desc[i];
    return s / double(k);
}

double p50_of(const std::vector<FrameRecord>& f, double FrameRecord::*m) {
    std::vector<double> v;
    for (const auto& r : f)
        if (std::isfinite(r.*m)) v.push_back(r.*m);
    return v.empty() ? kNaN : median(v);
}

double pct_of(const std::vector<FrameRecord>& f, double FrameRecord::*m, double p) {
    std::vector<double> v;
    for (const auto& r : f)
        if (std::isfinite(r.*m)) v.push_back(r.*m);
    return v.empty() ? kNaN : percentile(v, p);
}

}  // namespace

FrameStats compute_frame_stats(const FrameCapture& cap, const StutterParams& P) {
    FrameStats s;
    std::vector<double> ft;
    ft.reserve(cap.frames.size());
    for (const auto& f : cap.frames) ft.push_back(f.interval_ms);
    s.n_frames = ft.size();
    if (ft.empty()) return s;
    double total = 0.0;
    for (double v : ft) total += v;
    s.duration_s = total / 1000.0;
    s.ft_mean_ms = total / double(ft.size());
    s.avg_fps = 1000.0 / s.ft_mean_ms;

    std::vector<double> sorted = ft;
    std::sort(sorted.begin(), sorted.end());
    s.ft_p50_ms = percentile_sorted(sorted, 50);
    s.ft_p90_ms = percentile_sorted(sorted, 90);
    s.ft_p95_ms = percentile_sorted(sorted, 95);
    s.ft_p99_ms = percentile_sorted(sorted, 99);
    s.ft_p999_ms = percentile_sorted(sorted, 99.9);
    s.ft_max_ms = sorted.back();
    std::vector<double> desc(sorted.rbegin(), sorted.rend());
    s.low1_fps = 1000.0 / mean_of_slowest(desc, 0.01);
    s.low01_fps = 1000.0 / mean_of_slowest(desc, 0.001);
    s.p99_fps = 1000.0 / s.ft_p99_ms;

    // Stutters: frame time >= ratio × median of the previous window and >= median + min_excess.
    const size_t win = std::max<size_t>(5, size_t(std::lround(P.window_s * s.avg_fps)));
    std::vector<double> buf;
    for (size_t i = 1; i < ft.size(); ++i) {
        const size_t lo = i > win ? i - win : 0;
        buf.assign(ft.begin() + long(lo), ft.begin() + long(i));
        if (buf.size() < 5) continue;
        std::nth_element(buf.begin(), buf.begin() + long(buf.size() / 2), buf.end());
        const double med = buf[buf.size() / 2];
        if (ft[i] >= P.ratio * med && ft[i] - med >= P.min_excess_ms) s.stutter_frames.push_back(i);
    }
    s.stutter_count = int(s.stutter_frames.size());
    s.stutters_per_min = s.duration_s > 0 ? double(s.stutter_count) / (s.duration_s / 60.0) : 0.0;

    // Optional columns.
    size_t known = 0, dropped = 0;
    for (const auto& f : cap.frames)
        if (f.displayed >= 0) { ++known; if (f.displayed == 0) ++dropped; }
    if (known) s.dropped_pct = 100.0 * double(dropped) / double(known);
    {
        std::vector<double> d;
        for (const auto& f : cap.frames)
            if (std::isfinite(f.displayed_interval_ms) && f.displayed_interval_ms > 0) d.push_back(f.displayed_interval_ms);
        if (d.size() > 10) {
            double sum = 0.0;
            for (double v : d) sum += v;
            s.displayed_fps = 1000.0 * double(d.size()) / sum;
            s.displayed_p99_ms = percentile(d, 99);
        }
    }
    s.cpu_busy_p50_ms = p50_of(cap.frames, &FrameRecord::cpu_busy_ms);
    s.gpu_busy_p50_ms = p50_of(cap.frames, &FrameRecord::gpu_busy_ms);
    s.display_latency_p50_ms = p50_of(cap.frames, &FrameRecord::display_latency_ms);
    s.display_latency_p95_ms = pct_of(cap.frames, &FrameRecord::display_latency_ms, 95);
    s.until_displayed_p50_ms = p50_of(cap.frames, &FrameRecord::until_displayed_ms);
    s.pc_latency_p50_ms = p50_of(cap.frames, &FrameRecord::pc_latency_ms);
    s.click_to_photon_p50_ms = p50_of(cap.frames, &FrameRecord::click_to_photon_ms);

    if (std::isfinite(s.display_latency_p50_ms)) {
        s.latency_p50_ms = s.display_latency_p50_ms;
        s.latency_definition = "frame start to display";
    } else if (std::isfinite(s.until_displayed_p50_ms)) {
        s.latency_p50_ms = s.until_displayed_p50_ms;
        s.latency_definition = "present to display";
    }
    if (std::isfinite(s.gpu_busy_p50_ms) && s.gpu_busy_p50_ms >= 0.9 * s.ft_p50_ms) s.bound_hint = "gpu";
    else if (std::isfinite(s.cpu_busy_p50_ms) && s.cpu_busy_p50_ms >= 0.9 * s.ft_p50_ms) s.bound_hint = "cpu";
    else if (std::isfinite(s.gpu_busy_p50_ms) && std::isfinite(s.cpu_busy_p50_ms)) s.bound_hint = "cap-or-sync";
    return s;
}

RunMetrics run_metrics(const FrameStats& s) {
    RunMetrics m;
    m.avg_fps = s.avg_fps;
    m.low1_fps = s.low1_fps;
    m.p99_ms = s.ft_p99_ms;
    m.stutters_per_min = s.stutters_per_min;
    m.latency_p50_ms = s.latency_p50_ms;
    m.latency_definition = s.latency_definition;
    return m;
}

const char* to_string(MetricOutcome o) {
    switch (o) {
        case MetricOutcome::Insufficient: return "insufficient";
        case MetricOutcome::Better: return "better";
        case MetricOutcome::Worse: return "worse";
        case MetricOutcome::NoDifference: return "no-difference";
    }
    return "insufficient";
}

namespace {

std::vector<double> collect(const ConfigRuns& c, double RunMetrics::*m) {
    std::vector<double> v;
    for (const auto& r : c.runs)
        if (std::isfinite(r.*m)) v.push_back(r.*m);
    return v;
}

double cv(const std::vector<double>& v) {
    if (v.size() < 2) return kNaN;
    const double m = mean(v);
    return m != 0 ? stddev(v) / std::fabs(m) : kNaN;
}

}  // namespace

ConfigComparison compare_configs(const ConfigRuns& A, const ConfigRuns& B, const CompareThresholds& t) {
    ConfigComparison out;
    struct Spec { const char* key; const char* label; double RunMetrics::*m; bool higher; double pct_thr; double abs_thr; };
    const Spec specs[] = {
        {"avg_fps", "Average FPS", &RunMetrics::avg_fps, true, t.avg_fps_pct, 0.0},
        {"low1_fps", "1% low FPS", &RunMetrics::low1_fps, true, t.low1_pct, 0.0},
        {"p99_ms", "99th percentile frame time (ms)", &RunMetrics::p99_ms, false, t.p99_pct, 0.0},
        {"stutters_per_min", "Stutters per minute", &RunMetrics::stutters_per_min, false, 0.0, t.stutter_abs_per_min},
        {"latency_p50_ms", "Display latency, median (ms)", &RunMetrics::latency_p50_ms, false, t.latency_pct, 0.0},
    };
    for (const auto& sp : specs) {
        MetricComparison mc;
        mc.metric = sp.key;
        mc.label = sp.label;
        mc.higher_is_better = sp.higher;
        auto a = collect(A, sp.m), b = collect(B, sp.m);
        if (std::string(sp.key) == "latency_p50_ms") {
            // Only compare latency figures that follow the same definition.
            std::set<std::string> defs;
            for (const auto& r : A.runs) if (std::isfinite(r.latency_p50_ms)) defs.insert(r.latency_definition);
            for (const auto& r : B.runs) if (std::isfinite(r.latency_p50_ms)) defs.insert(r.latency_definition);
            if (defs.size() > 1) {
                out.notes.push_back("Latency was measured with different PresentMon definitions and is not compared.");
                continue;
            }
        }
        mc.n_a = int(a.size());
        mc.n_b = int(b.size());
        if (a.empty() || b.empty()) {
            if (std::string(sp.key) != "latency_p50_ms") out.metrics.push_back(mc);
            continue;
        }
        mc.a_mean = mean(a);
        mc.b_mean = mean(b);
        mc.a_cv = cv(a);
        mc.b_cv = cv(b);
        mc.diff = mc.b_mean - mc.a_mean;
        mc.diff_pct = mc.a_mean != 0 ? 100.0 * mc.diff / std::fabs(mc.a_mean) : kNaN;
        const auto w = welch_t(a, b);
        if (!w.valid) {
            mc.outcome = MetricOutcome::Insufficient;
        } else {
            mc.ci = w.ci95;
            const bool sig = !(w.ci95.lo <= 0.0 && w.ci95.hi >= 0.0);
            const bool big = sp.abs_thr > 0.0 ? std::fabs(mc.diff) >= sp.abs_thr : std::fabs(mc.diff_pct) >= sp.pct_thr;
            if (sig && big) mc.outcome = ((mc.diff > 0) == sp.higher) ? MetricOutcome::Better : MetricOutcome::Worse;
            else mc.outcome = MetricOutcome::NoDifference;
        }
        out.metrics.push_back(mc);
    }
    for (const auto& mc : out.metrics) {
        if (mc.metric == "avg_fps" && ((std::isfinite(mc.a_cv) && mc.a_cv > t.noisy_cv_avg_fps) ||
                                       (std::isfinite(mc.b_cv) && mc.b_cv > t.noisy_cv_avg_fps)))
            out.noisy = true;
        if ((mc.metric == "low1_fps" || mc.metric == "p99_ms") &&
            ((std::isfinite(mc.a_cv) && mc.a_cv > t.noisy_cv_low) || (std::isfinite(mc.b_cv) && mc.b_cv > t.noisy_cv_low)))
            out.noisy = true;
    }
    out.preliminary = int(A.runs.size()) < t.min_runs || int(B.runs.size()) < t.min_runs;
    if (out.noisy)
        out.notes.push_back("Runs of the same configuration differ a lot. Make the runs more repeatable (same spot, "
                            "same route, same duration, nothing else running) before trusting small differences.");
    if (out.preliminary)
        out.notes.push_back("Fewer than " + std::to_string(t.min_runs) + " runs per configuration: treat this as preliminary.");

    int better = 0, worse = 0;
    for (const auto& mc : out.metrics) {
        if (mc.outcome == MetricOutcome::Better) ++better;
        if (mc.outcome == MetricOutcome::Worse) ++worse;
    }
    if (out.preliminary && (A.runs.size() < 2 || B.runs.size() < 2))
        out.summary = "Record at least two runs of each configuration to compare them.";
    else if (better == 0 && worse == 0)
        out.summary = "No measurable difference beyond run-to-run variation.";
    else if (worse == 0)
        out.summary = "Better on " + std::to_string(better) + " metric(s), worse on none.";
    else if (better == 0)
        out.summary = "Worse on " + std::to_string(worse) + " metric(s), better on none.";
    else
        out.summary = "A trade-off: better on " + std::to_string(better) + " metric(s), worse on " + std::to_string(worse) + ".";
    return out;
}

FrameRecommendation recommend_config(const std::vector<ConfigRuns>& configs, FrameGoal goal, const CompareThresholds& t) {
    FrameRecommendation rec;
    if (configs.size() < 2) {
        rec.reason = "Add at least one candidate configuration to compare against the baseline.";
        return rec;
    }
    const char* key = goal == FrameGoal::Consistency ? "p99_ms" : goal == FrameGoal::Throughput ? "avg_fps" : "latency_p50_ms";
    double best_gain = 0.0;
    for (size_t i = 1; i < configs.size(); ++i) {
        const auto cmp = compare_configs(configs[0], configs[i], t);
        if (cmp.noisy || cmp.preliminary) continue;
        const MetricComparison* goal_m = nullptr;
        bool harmful = false;
        for (const auto& mc : cmp.metrics) {
            if (mc.metric == key) goal_m = &mc;
            // A recommendation must not make consistency worse.
            if ((mc.metric == "p99_ms" || mc.metric == "low1_fps" || mc.metric == "stutters_per_min") &&
                mc.outcome == MetricOutcome::Worse)
                harmful = true;
        }
        if (!goal_m || goal_m->outcome != MetricOutcome::Better || harmful) continue;
        const double gain = std::fabs(goal_m->diff_pct);
        if (gain > best_gain) {
            best_gain = gain;
            rec.best = int(i);
            rec.reliable = true;
        }
    }
    if (!rec.reliable) {
        rec.best = 0;
        rec.reason = "No candidate beat the baseline reliably on this goal without hurting consistency. Keep the baseline.";
    } else {
        // Say which number improved, in words a player knows.
        const char* what = goal == FrameGoal::Consistency ? "made your slowest frames %.1f%% quicker (99th percentile frame time)"
                           : goal == FrameGoal::Throughput ? "raised your average FPS by %.1f%%"
                                                           : "lowered the median latency PresentMon reports by %.1f%% (this is "
                                                             "not full mouse-to-screen latency)";
        char part[192];
        std::snprintf(part, sizeof part, what, best_gain);
        char buf[384];
        std::snprintf(buf, sizeof buf, "\"%s\" %s over repeated runs, without making frame-time consistency worse.",
                      configs[size_t(rec.best)].label.c_str(), part);
        rec.reason = buf;
    }
    return rec;
}

}  // namespace fit
