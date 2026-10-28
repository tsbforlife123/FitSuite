#include <cmath>
#include <sstream>

#include "doctest.h"
#include "fit/frame.hpp"

using namespace fit;

namespace {

const char* kV1Header =
    "Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,PresentFlags,Dropped,TimeInSeconds,msInPresentAPI,"
    "msBetweenPresents,AllowsTearing,PresentMode,msUntilRenderComplete,msUntilDisplayed,msBetweenDisplayChange,msFlipDelay,"
    "msUntilRenderStart,msGPUActive,msSinceInput";
const char* kV2Header =
    "Application,ProcessID,SwapChainAddress,PresentRuntime,SyncInterval,PresentFlags,AllowsTearing,PresentMode,CPUStartTime,"
    "FrameTime,CPUBusy,CPUWait,GPULatency,GPUTime,GPUBusy,GPUWait,DisplayLatency,DisplayedTime,AnimationError,AnimationTime,"
    "MsFlipDelay,AllInputToPhotonLatency,ClickToPhotonLatency";
const char* kNewHeader =
    "Application,ProcessID,SwapChainAddress,PresentRuntime,SyncInterval,PresentFlags,AllowsTearing,PresentMode,TimeInSeconds,"
    "MsBetweenSimulationStart,MsBetweenPresents,MsBetweenDisplayChange,MsInPresentAPI,MsRenderPresentLatency,MsUntilDisplayed,"
    "CPUStartTimeInSeconds,MsBetweenAppStart,MsCPUBusy,MsCPUWait,MsGPULatency,MsGPUTime,MsGPUBusy,MsGPUWait,MsAnimationError,"
    "AnimationTime,MsFlipDelay,MsAllInputToPhotonLatency,MsClickToPhotonLatency";

std::string v1_csv(const std::vector<double>& ft, const char* app = "VALORANT-Win64-Shipping.exe") {
    std::ostringstream o;
    o << kV1Header << "\r\n";
    double t = 0;
    for (double f : ft) {
        t += f / 1000.0;
        o << app << ",1234,0x0000000012345678,DXGI,0,512,0," << t << ",0.10," << f
          << ",1,Hardware: Independent Flip,1.5,2.0," << f << ",0.0,0.2,2.5,NA\r\n";
    }
    return o.str();
}

std::string v2_csv(const std::vector<double>& ft) {
    std::ostringstream o;
    o << kV2Header << "\n";
    double t = 0;
    for (double f : ft) {
        o << "VALORANT-Win64-Shipping.exe,1234,0x12345678,DXGI,0,512,1,Hardware: Independent Flip," << t << "," << f << ","
          << f * 0.6 << "," << f * 0.4 << ",0.3," << f * 0.9 << "," << f * 0.85 << "," << f * 0.15 << ",6.5," << f
          << ",0.0," << t << ",0.0,NA,NA\n";
        t += f;
    }
    return o.str();
}

std::string new_csv(const std::vector<double>& ft) {
    std::ostringstream o;
    o << kNewHeader << "\n";
    double t = 0;
    for (double f : ft) {
        o << "VALORANT-Win64-Shipping.exe,1234,0x12345678,DXGI,0,512,1,Hardware: Independent Flip," << t / 1000 << "," << f << ","
          << f << "," << f << ",0.1,1.0,1.8," << t / 1000 << "," << f << "," << f * 0.5 << "," << f * 0.5 << ",0.2,"
          << f * 0.97 << "," << f * 0.96 << "," << f * 0.04 << ",0.0," << t << ",0.0,NA,NA\n";
        t += f;
    }
    return o.str();
}

std::vector<double> constant(double ms, int n) { return std::vector<double>(size_t(n), ms); }

}  // namespace

TEST_CASE("parses PresentMon 1.x, 2.x and current default CSV layouts") {
    const auto ft = constant(4.0, 500);
    std::string err;
    auto c1 = parse_presentmon_csv(v1_csv(ft), {}, &err);
    REQUIRE(c1);
    CHECK(c1->source_format == "PresentMon 1.x metrics");
    CHECK(c1->frames.size() == 500);
    CHECK(c1->column_used["frame_interval"] == "msBetweenPresents");
    CHECK(c1->frames[0].displayed == 1);
    CHECK(c1->present_mode == "Hardware: Independent Flip");

    auto c2 = parse_presentmon_csv(v2_csv(ft), {}, &err);
    REQUIRE(c2);
    CHECK(c2->source_format == "PresentMon 2.x metrics");
    CHECK(c2->column_used["frame_interval"] == "FrameTime");
    CHECK(c2->frames[0].gpu_busy_ms == doctest::Approx(3.4));
    CHECK(std::isnan(c2->frames[0].click_to_photon_ms));  // "NA" -> not available

    auto c3 = parse_presentmon_csv(new_csv(ft), {}, &err);
    REQUIRE(c3);
    CHECK(c3->source_format == "PresentMon 2.4+ (default metrics)");
    CHECK(c3->column_used["frame_interval"] == "MsBetweenPresents");
    CHECK(c3->column_used["cpu_busy"] == "MsCPUBusy");
}

TEST_CASE("rejects files that are not PresentMon captures") {
    std::string err;
    CHECK_FALSE(parse_presentmon_csv("a,b,c\n1,2,3\n", {}, &err));
    CHECK(err.find("PresentMon") != std::string::npos);
    CHECK_FALSE(parse_presentmon_csv("", {}, &err));
}

TEST_CASE("selects the requested process and the busiest swap chain") {
    std::string text = v1_csv(constant(4.0, 300));
    const std::string other = v1_csv(constant(16.0, 900), "dwm.exe");
    text += other.substr(other.find('\n') + 1);
    std::string err;
    auto c = parse_presentmon_csv(text, {"VALORANT-Win64-Shipping.exe"}, &err);
    REQUIRE(c);
    CHECK(c->frames.size() == 300);
    CHECK(c->other_applications.size() == 1);
    auto any = parse_presentmon_csv(text, {}, &err);
    REQUIRE(any);
    CHECK(any->application == "dwm.exe");  // most frames when no process is requested
    CHECK_FALSE(parse_presentmon_csv(text, {"notepad.exe"}, &err));
}

TEST_CASE("frame statistics: units and definitions") {
    // 1000 frames at 4 ms with 10 frames at 20 ms.
    std::vector<double> ft = constant(4.0, 1000);
    for (int i = 0; i < 10; ++i) ft[size_t(100 + i * 90)] = 20.0;
    std::string err;
    auto c = parse_presentmon_csv(v1_csv(ft), {}, &err);
    REQUIRE(c);
    const auto s = compute_frame_stats(*c);
    const double total_ms = 990 * 4.0 + 10 * 20.0;
    CHECK(s.n_frames == 1000);
    CHECK(s.duration_s == doctest::Approx(total_ms / 1000.0));
    CHECK(s.avg_fps == doctest::Approx(1000.0 * 1000.0 / total_ms));  // frames / time, not mean of FPS
    CHECK(s.ft_p50_ms == doctest::Approx(4.0));
    CHECK(s.ft_max_ms == doctest::Approx(20.0));
    // Slowest 1% = 10 frames, all 20 ms -> 50 FPS.
    CHECK(s.low1_fps == doctest::Approx(50.0));
    CHECK(s.stutter_count == 10);
    CHECK(s.stutters_per_min == doctest::Approx(10.0 / (total_ms / 60000.0)));
    CHECK(s.dropped_pct == doctest::Approx(0.0));
}

TEST_CASE("a steady frame rate has no stutters; a slow drift is not a stutter") {
    std::vector<double> ft;
    for (int i = 0; i < 3000; ++i) ft.push_back(4.0 + 3.0 * double(i) / 3000.0);  // 250 -> ~143 fps slowly
    std::string err;
    auto c = parse_presentmon_csv(v1_csv(ft), {}, &err);
    REQUIRE(c);
    CHECK(compute_frame_stats(*c).stutter_count == 0);
}

TEST_CASE("2.x metrics: CPU/GPU timing and latency are reported only when present") {
    std::string err;
    auto c = parse_presentmon_csv(v2_csv(constant(5.0, 400)), {}, &err);
    REQUIRE(c);
    const auto s = compute_frame_stats(*c);
    CHECK(s.gpu_busy_p50_ms == doctest::Approx(4.25));
    CHECK(s.cpu_busy_p50_ms == doctest::Approx(3.0));
    CHECK(s.bound_hint == "cap-or-sync");  // GPU busy 85% < 90% of the frame
    CHECK(s.display_latency_p50_ms == doctest::Approx(6.5));
    CHECK(s.latency_definition == "frame start to display");
    CHECK(std::isnan(s.click_to_photon_p50_ms));

    auto c1 = parse_presentmon_csv(v1_csv(constant(5.0, 400)), {}, &err);
    const auto s1 = compute_frame_stats(*c1);
    CHECK(std::isnan(s1.cpu_busy_p50_ms));
    CHECK(s1.latency_definition == "present to display");
}

TEST_CASE("run comparison: noise flags, thresholds and recommendations") {
    auto runs = [](std::initializer_list<double> fps, double p99) {
        ConfigRuns c;
        for (double f : fps) {
            RunMetrics m;
            m.avg_fps = f;
            m.low1_fps = f * 0.6;
            m.p99_ms = p99;
            m.stutters_per_min = 0.5;
            c.runs.push_back(m);
        }
        return c;
    };
    ConfigRuns base = runs({300, 302, 298}, 6.0);
    base.label = "Uncapped";
    ConfigRuns same = runs({301, 299, 300}, 6.0);
    auto cmp = compare_configs(base, same);
    CHECK_FALSE(cmp.noisy);
    for (const auto& m : cmp.metrics)
        if (m.metric == "avg_fps") CHECK(m.outcome == MetricOutcome::NoDifference);

    ConfigRuns faster = runs({330, 332, 329}, 6.0);
    cmp = compare_configs(base, faster);
    for (const auto& m : cmp.metrics)
        if (m.metric == "avg_fps") CHECK(m.outcome == MetricOutcome::Better);

    ConfigRuns noisy = runs({250, 340, 300}, 6.0);
    cmp = compare_configs(base, noisy);
    CHECK(cmp.noisy);

    ConfigRuns two = runs({330, 331}, 6.0);
    CHECK(compare_configs(base, two).preliminary);

    // A cap that improves consistency (lower p99) is recommended for the consistency goal.
    ConfigRuns capped;
    capped.label = "Cap 237";
    for (double p : {4.9, 5.0, 5.05}) {
        RunMetrics m;
        m.avg_fps = 236;
        m.low1_fps = 200;
        m.p99_ms = p;
        m.stutters_per_min = 0.2;
        capped.runs.push_back(m);
    }
    auto rec = recommend_config({base, capped}, FrameGoal::Consistency);
    CHECK(rec.reliable);
    CHECK(rec.best == 1);
    // ...but not for raw throughput.
    rec = recommend_config({base, capped}, FrameGoal::Throughput);
    CHECK_FALSE(rec.reliable);
    CHECK(rec.best == 0);
}
