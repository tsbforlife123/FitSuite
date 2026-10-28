// FitSuite Linux development harness: renders the real UI code with X11 + GLX + OpenGL3 and drives
// it with a scripted, simulated player. Used to develop and screenshot the interface on machines
// without Windows. Every result it produces is labelled "simulated".
//
//   fitsuite_harness --data <dir> [--size 1600x1000] [commands…]
//
// Commands run in order:
//   page=<mouse|framefit|motionfit|profiles|nerd|settings> nerdtab=<n> tab=<n> frames=<n> shot=<file.png>
//   dpi=<n> sens=<x> user=<unbiased|vertical|down|strong|rotated> calibrate[=<blocks>] validate activate
//   motion-refresh=<hz,hz> trails=<a,b> gen-runs=<label>|<fps>|<runs>|<jitter> compare=<labelA>|<labelB>
//   focusloss quit
//   Raw Accel and games: fake-rawaccel[=<dir>] ra-export foreground=<exe> running=<exe:title,…>
//   games=<exe,…> games-on games-off confirm-everywhere writer-fails=<exit code> print-games
//   Settings: theme=<light|dark|system> system-dark=<0|1> extras=<0|1> model=<text> reminder=<days>
//   startup=<0|1> startup-blocked=<0|1> game-sens=<exe>|<sensitivity>|<cm360> diag
//   Quick check: check age-fix=<days> reminder-now print-check
//   Refresh rate: refresh-switch refresh-keep print-refresh
//   Setup: setup-reset[=step] setup-step=<n> setup-finish fake-download=<url>|<file> vc-runtime=<0|1> ra-install
//   reboot ra-apply print-setup
//   Updates: update-check update-download update-install print-update
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app.hpp"
#include "fit/rng.hpp"
#include "fit/timeutil.hpp"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "platform_harness.hpp"
#include "sim_user.hpp"
#include "theme.hpp"

// X11 last: its macros (None, Bool, Status, …) would clash with project headers.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dlfcn.h>

namespace {

// ---- minimal GLX / GL declarations (the build machine has no GL headers) ----------------------
typedef struct __GLXcontextRec* GLXContext;
using PFNglXChooseVisual = XVisualInfo* (*)(Display*, int, int*);
using PFNglXCreateContext = GLXContext (*)(Display*, XVisualInfo*, GLXContext, Bool);
using PFNglXMakeCurrent = Bool (*)(Display*, XID, GLXContext);
using PFNglXSwapBuffers = void (*)(Display*, XID);
using PFNglXGetProcAddressARB = void* (*)(const unsigned char*);
constexpr int kGLX_RGBA = 4, kGLX_DOUBLEBUFFER = 5, kGLX_RED_SIZE = 8, kGLX_GREEN_SIZE = 9, kGLX_BLUE_SIZE = 10;
using PFNglViewport = void (*)(int, int, int, int);
using PFNglClearColor = void (*)(float, float, float, float);
using PFNglClear = void (*)(unsigned);
using PFNglReadPixels = void (*)(int, int, int, int, unsigned, unsigned, void*);
using PFNglPixelStorei = void (*)(unsigned, int);
using PFNglFinish = void (*)();

struct GL {
    PFNglXChooseVisual ChooseVisual;
    PFNglXCreateContext CreateContext;
    PFNglXMakeCurrent MakeCurrent;
    PFNglXSwapBuffers SwapBuffers;
    PFNglXGetProcAddressARB GetProcAddress;
    PFNglViewport Viewport;
    PFNglClearColor ClearColor;
    PFNglClear Clear;
    PFNglReadPixels ReadPixels;
    PFNglPixelStorei PixelStorei;
    PFNglFinish Finish;
} gl;

bool load_gl() {
    void* lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) return false;
    gl.ChooseVisual = (PFNglXChooseVisual)dlsym(lib, "glXChooseVisual");
    gl.CreateContext = (PFNglXCreateContext)dlsym(lib, "glXCreateContext");
    gl.MakeCurrent = (PFNglXMakeCurrent)dlsym(lib, "glXMakeCurrent");
    gl.SwapBuffers = (PFNglXSwapBuffers)dlsym(lib, "glXSwapBuffers");
    gl.GetProcAddress = (PFNglXGetProcAddressARB)dlsym(lib, "glXGetProcAddressARB");
    auto get = [&](const char* n) { return gl.GetProcAddress((const unsigned char*)n); };
    gl.Viewport = (PFNglViewport)get("glViewport");
    gl.ClearColor = (PFNglClearColor)get("glClearColor");
    gl.Clear = (PFNglClear)get("glClear");
    gl.ReadPixels = (PFNglReadPixels)get("glReadPixels");
    gl.PixelStorei = (PFNglPixelStorei)get("glPixelStorei");
    gl.Finish = (PFNglFinish)get("glFinish");
    return gl.ChooseVisual && gl.CreateContext && gl.MakeCurrent && gl.SwapBuffers && gl.Viewport && gl.ReadPixels;
}

void screenshot(int w, int h, const std::string& path) {
    std::vector<unsigned char> px(size_t(w) * h * 4);
    gl.Finish();
    gl.PixelStorei(0x0D05, 1);
    gl.ReadPixels(0, 0, w, h, 0x1908, 0x1401, px.data());
    const std::string ppm = path + ".ppm";
    std::ofstream f(ppm, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    for (int y = h - 1; y >= 0; --y)
        for (int x = 0; x < w; ++x) f.write(reinterpret_cast<const char*>(&px[(size_t(y) * w + x) * 4]), 3);
    f.close();
    const std::string cmd = "convert '" + ppm + "' '" + path + "' && rm -f '" + ppm + "'";
    if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "screenshot conversion failed: %s\n", path.c_str());
    else std::printf("screenshot %s\n", path.c_str());
}

// ---- Raw Accel 1.7 settings.json as written by its GUI (no acceleration) -----------------------
const char* const kSampleRawAccelSettings = R"json({
  "### Accel modes ###": "classic | jump | natural | synchronous | power | lut | noaccel",
  "version": "1.7.0",
  "defaultDeviceConfig": {
    "disable": false,
    "DPI (normalizes input speed unit: counts/ms -> in/s)": 0
  },
  "profiles": [
    {
      "name": "default",
      "Whole or horizontal accel parameters": { "mode": "noaccel", "Gain / Velocity": true },
      "Vertical accel parameters": { "mode": "noaccel", "Gain / Velocity": true },
      "Output DPI": 1000.0,
      "Y/X output DPI ratio (vertical sens multiplier)": 1.0,
      "L/R output DPI ratio (left sens multiplier)": 1.0,
      "U/D output DPI ratio (up sens multiplier)": 1.0,
      "Degrees of rotation": 0.0,
      "Degrees of angle snapping": 0.0,
      "Input Speed Cap": 0.0
    }
  ],
  "devices": []
}
)json";

// ---- synthetic PresentMon CSV (labelled simulated when imported) ------------------------------
void write_synthetic_csv(const std::filesystem::path& p, double fps, double jitter, uint64_t seed) {
    fit::Rng r(seed);
    std::ofstream f(p);
    f << "Application,ProcessID,SwapChainAddress,PresentRuntime,SyncInterval,PresentFlags,AllowsTearing,PresentMode,"
         "CPUStartTime,FrameTime,CPUBusy,CPUWait,GPULatency,GPUTime,GPUBusy,GPUWait,DisplayLatency,DisplayedTime,"
         "AnimationError,AnimationTime,MsFlipDelay,AllInputToPhotonLatency,ClickToPhotonLatency\n";
    double t = 0.0;
    const double base = 1000.0 / fps;
    while (t < 60000.0) {
        double ft = base * std::exp(r.normal(0.0, jitter));
        if (r.uniform() < 0.002) ft *= 3.0 + r.uniform() * 3.0;  // occasional hitch
        f << "VALORANT-Win64-Shipping.exe,4242,0x1,DXGI,0,512,1,Hardware: Independent Flip," << t << "," << ft << ","
          << ft * 0.55 << "," << ft * 0.45 << ",0.2," << ft * 0.8 << "," << ft * 0.78 << "," << ft * 0.22 << ","
          << ft * 1.9 << "," << ft << ",0.0," << t << ",0.0,NA,NA\n";
        t += ft;
    }
}

std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> out;
    size_t a = 0;
    while (true) {
        const size_t b = s.find(c, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

// ---- automation -----------------------------------------------------------------------------
struct Runner {
    std::vector<std::pair<std::string, std::string>> cmds;
    size_t pc = 0;
    int wait = 0;
    bool driving = false;  // a test is running under simulation
    fit::sim::SimUserParams user;
    std::unique_ptr<fit::sim::SimUser> sim;
    uint64_t last_serial = 0;
    int64_t break_since = 0;
    fit::Rng observer{99};
    std::string pending_shot;
    bool quit = false;
    int focus_frames = 0;
    std::vector<std::pair<int, std::string>> test_shots;  // frames into the next test -> file
    int test_frame = 0;

    void push_trial(fitapp::HarnessPlatform& plat, const fit::Trial& tr) {
        for (const auto& m : tr.motion) {
            fitapp::RawEvent e;
            e.t_us = m.t_us;
            e.dx = m.in_dx;
            e.dy = m.in_dy;
            e.device_id = "sim-mouse";
            plat.sim_mouse().push(e);
        }
        for (const auto& c : tr.clicks) {
            fitapp::RawEvent e;
            e.t_us = c.t_us;
            e.buttons = fitapp::kLeftDown;
            e.device_id = "sim-mouse";
            plat.sim_mouse().push(e);
        }
    }

    void drive_mouse(fitapp::App& app, fitapp::HarnessPlatform& plat) {
        auto& t = *app.mouse.test;
        using P = fitapp::MouseTest::Phase;
        if (t.phase() == P::Instructions) t.begin();
        if (t.phase() == P::RoundIntro || t.phase() == P::Paused) {
            if (!break_since) break_since = plat.clock_us;
            if (plat.clock_us - break_since > 600000) {
                t.resume();
                break_since = 0;
            }
            return;
        }
        if (!sim) {
            fit::sim::SimUserParams p = user;
            p.single_click = true;  // round-based tests: one shot per target
            if (p.settle_deg == 0.0) p.settle_deg = 0.3;
            sim = std::make_unique<fit::sim::SimUser>(p, 4242);
        }
        const fitapp::MouseTestProbe pr = t.probe();
        if (pr.tracking) {
            const double dt = plat.frame_dt_ms / 1000.0;
            const fit::Vec2 c = sim->track_step(double(plat.clock_us) / 1e6, dt, pr.track_yaw, pr.track_pitch, pr.track_vyaw,
                                                pr.track_vpitch, pr.yaw_deg, pr.pitch_deg, pr.mapping);
            if (c.x != 0.0 || c.y != 0.0) {
                fitapp::RawEvent e;
                e.t_us = plat.clock_us;
                e.dx = int32_t(c.x);
                e.dy = int32_t(c.y);
                e.device_id = "sim-mouse";
                plat.sim_mouse().push(e);
            }
            return;
        }
        if (!pr.target_active || pr.trial_serial == last_serial) return;
        last_serial = pr.trial_serial;
        fit::TargetSpec spec = pr.spec;
        if (pr.task == fit::Task::Switch) {  // go for the nearest target
            double best = 1e9;
            for (const auto& g : pr.switch_targets) {
                const double d = std::hypot(g.yaw - pr.yaw_deg, g.pitch - pr.pitch_deg);
                if (d < best) {
                    best = d;
                    spec.dir_deg = std::atan2(g.pitch - pr.pitch_deg, g.yaw - pr.yaw_deg) * fit::kRadToDeg;
                    spec.dist_deg = d;
                    spec.radius_deg = g.radius;
                }
            }
            if (best > 1e8) return;
        }
        sim->params().timeout_ms = 2000.0;
        push_trial(plat, sim->run_trial(spec, pr.mapping, pr.active_transform, pr.yaw_deg, pr.pitch_deg, pr.t_appear_us + 2000));
    }

    void drive_motion(fitapp::App& app, fitapp::HarnessPlatform& plat) {
        auto& t = *app.motion.test;
        using P = fitapp::MotionTest::Phase;
        switch (t.phase()) {
            case P::Instructions: t.begin(); break;
            case P::AwaitManual: t.continue_manual(); break;
            case P::BlockBreak: t.continue_block(); break;
            case P::Respond: {
                const fit::MotionTrialSpec* s = t.current_spec();
                if (!s) break;
                // Simulated observer: details stay readable while motion blur (speed / rate) is below ~10 px.
                const double hz = plat.refresh_hz();
                const double th = 10.0 * hz / 1600.0;
                const double z = (std::log2(s->speed_sw_s) - std::log2(th)) / 0.35;
                const double p = 0.25 + 0.75 * 0.98 / (1.0 + std::exp(z - 1.1));
                const bool ok = observer.uniform() < p;
                t.answer(ok ? s->orientation : (s->orientation + 1 + int(observer.below(3))) % 4);
                break;
            }
            case P::Rate: t.rate(int(observer.below(3)), int(observer.below(2))); break;
            default: break;
        }
    }

    // Returns false when the harness should exit.
    bool pre_frame(fitapp::App& app, fitapp::HarnessPlatform& plat) {
        if (focus_frames > 0 && --focus_frames == 0) plat.set_focused(true);
        if (driving) {
            ++test_frame;
            for (auto it = test_shots.begin(); it != test_shots.end(); ++it)
                if (it->first == test_frame) {
                    pending_shot = it->second;
                    test_shots.erase(it);
                    break;
                }
            if (app.mouse.test) drive_mouse(app, plat);
            else if (app.motion.test) drive_motion(app, plat);
            else {
                driving = false;
                test_frame = 0;
            }
            return true;
        }
        if (wait > 0) {
            --wait;
            return true;
        }
        while (pc < cmds.size() && wait == 0 && !driving && pending_shot.empty()) {
            const auto [k, v] = cmds[pc++];
            auto& setup = app.setup();
            if (k == "page") {
                const fitapp::Page p = v == "mousefit" || v == "mouse" ? fitapp::Page::MouseFit : v == "framefit" ? fitapp::Page::FrameFit
                                       : v == "motionfit" ? fitapp::Page::MotionFit : v == "profiles" ? fitapp::Page::Profiles
                                       : v == "nerd" ? fitapp::Page::Nerd : v == "settings" ? fitapp::Page::Settings
                                                                                              : fitapp::Page::Overview;
                app.go(p);
                wait = 3;
            } else if (k == "nerdtab") {
                app.nerd_tab = std::atoi(v.c_str());
                wait = 3;
            } else if (k == "tab") {
                app.mouse.tab = std::atoi(v.c_str());
                wait = 3;
            } else if (k == "frames") {
                wait = std::atoi(v.c_str());
            } else if (k == "shot") {
                pending_shot = v;
            } else if (k == "testshot") {
                const auto parts = split(v, '|');
                if (parts.size() == 2) test_shots.push_back({std::atoi(parts[0].c_str()), parts[1]});
            } else if (k == "dpi") {
                setup.mouse.dpi = std::atoi(v.c_str());
                app.save();
            } else if (k == "sens") {
                setup.game.sensitivity = std::atof(v.c_str());
                app.save();
            } else if (k == "overdrive") {
                setup.display.overdrive = v;
                app.save();
            } else if (k == "user") {
                user = {};
                if (v == "vertical") user.m11 = 0.90;
                if (v == "down") user.down_gain = 0.88;  // flicks toward the bottom of the screen land 12% short
                if (v == "strong") {  // precise player whose only error is a clear vertical undershoot
                    user.m11 = 0.86;
                    user.primary_gain = 0.99;
                    user.radial_sd = 0.045;
                    user.angle_sd_deg = 1.5;
                }
                if (v == "rotated") {
                    const double a = 3.0 * fit::kDegToRad;
                    user.m00 = std::cos(a);
                    user.m01 = std::sin(a);
                    user.m10 = -std::sin(a);
                    user.m11 = std::cos(a);
                }
                sim.reset();
            } else if (k == "calibrate") {
                if (!v.empty()) app.mouse.calib_blocks = std::atoi(v.c_str());
                app.start_calibration();
                driving = true;
            } else if (k == "validate") {
                // Like the simple view: the Raw Accel-compatible fix from the latest calibration.
                std::string id;
                for (const auto& m : setup.mouse_profiles)
                    if (m.status != "discarded" && m.transform.kind == fit::TransformKind::RawAccel &&
                        (!app.mouse.calibration || m.calibration_session == app.mouse.calibration->id))
                        id = m.id;
                if (id.empty()) id = app.mouse.selected_profile;
                if (!v.empty()) app.mouse.val_blocks = std::atoi(v.c_str());
                if (!id.empty()) {
                    app.start_validation(id);
                    driving = true;
                }
            } else if (k == "motion-refresh" || k == "trails" || k == "motion-od") {
                fitapp::MotionTestConfig cfg;
                cfg.mode = k == "trails" ? fitapp::MotionTestConfig::Mode::Trails : fitapp::MotionTestConfig::Mode::Identification;
                for (const auto& item : split(v, ',')) {
                    fit::MotionConfig c;
                    if (k == "motion-refresh") {
                        c.refresh_hz = std::atof(item.c_str());
                        c.label = item + " Hz";
                        c.app_controlled = true;
                    } else {
                        c.overdrive = item;
                        c.label = "Overdrive " + item;
                        c.refresh_hz = 144;
                    }
                    cfg.configs.push_back(c);
                }
                fit::MotionPlanOptions po;
                po.seed = 7;
                cfg.plan = fit::make_motion_plan(int(cfg.configs.size()), po);
                cfg.display_device = setup.display.device;
                app.start_motion_test(cfg);
                driving = true;
            } else if (k == "gen-runs") {
                const auto parts = split(v, '|');
                const std::string label = parts[0];
                const double fps = parts.size() > 1 ? std::atof(parts[1].c_str()) : 240;
                const int n = parts.size() > 2 ? std::atoi(parts[2].c_str()) : 3;
                const double jitter = parts.size() > 3 ? std::atof(parts[3].c_str()) : 0.08;
                fit::FrameConfig* cfg = nullptr;
                for (auto& c : setup.frame_configs)
                    if (c.label == label) cfg = &c;
                if (!cfg && label == "Baseline")  // the default baseline configuration
                    for (auto& c : setup.frame_configs)
                        if (c.baseline) cfg = &c;
                if (!cfg) {
                    fit::FrameConfig c;
                    c.id = fit::make_id("cfg");
                    c.label = label;
                    c.baseline = setup.frame_configs.empty();
                    setup.frame_configs.push_back(c);
                    cfg = &setup.frame_configs.back();
                }
                const std::string id = cfg->id;
                app.frame_ui.selected_config = id;
                std::filesystem::create_directories(plat.data_dir() / "harness-tmp");
                for (int i = 0; i < n; ++i) {
                    const auto p = plat.data_dir() / "harness-tmp" / (fit::make_id("csv") + ".csv");
                    write_synthetic_csv(p, fps * (1.0 + observer.normal(0.0, 0.01)), jitter, observer.next_u64());
                    fitapp::framefit_import(app, id, p, "simulated");
                }
            } else if (k == "select-config") {
                for (const auto& c : setup.frame_configs)
                    if (c.label == v) app.frame_ui.selected_config = c.id;
            } else if (k == "compare") {
                const auto parts = split(v, '|');
                for (const auto& c : setup.frame_configs) {
                    if (c.label == parts[0]) app.frame_ui.compare_a = c.id;
                    if (parts.size() > 1 && c.label == parts[1]) app.frame_ui.compare_b = c.id;
                }
            } else if (k == "activate") {  // UI review shortcut: mark the selected candidate as the chosen correction
                for (auto& m : setup.mouse_profiles)
                    if (m.id == app.mouse.selected_profile || (app.mouse.selected_profile.empty() && m.status != "discarded")) {
                        setup.active_mouse_profile = m.id;
                        m.status = "active";
                        break;
                    }
                app.save();
            } else if (k == "ra-export") {  // same as "Create a Raw Accel settings file…" with this settings.json
                if (auto* m = app.mouse_profile(setup.active_mouse_profile))
                    fitapp::rawaccel_export(app, m->transform.raw_accel, v);
                app.mouse.tab = 4;
                wait = 3;
            } else if (k == "fake-rawaccel") {  // a Raw Accel folder with settings.json and a stand-in writer.exe
                namespace fs = std::filesystem;
                const fs::path dir = v.empty() ? plat.data_dir() / "harness-rawaccel" : fs::path(v);
                fs::create_directories(dir);
                std::ofstream(dir / "settings.json") << kSampleRawAccelSettings;
                std::ofstream(dir / "writer.exe") << "stand-in";
                std::ofstream(plat.data_dir() / "rawaccel-settings-path.txt") << (dir / "settings.json").string();
                plat.rawaccel_present = true;
            } else if (k == "foreground") {
                plat.foreground = v;
            } else if (k == "running") {  // exe:title,exe:title
                plat.running.clear();
                for (const auto& item : split(v, ',')) {
                    const auto kv = split(item, ':');
                    plat.running.push_back({kv[0], kv.size() > 1 ? kv[1] : kv[0]});
                }
            } else if (k == "games") {
                setup.fix_games.games = v.empty() ? std::vector<std::string>{} : split(v, ',');
                app.save();
            } else if (k == "games-on") {  // same as "Turn it on" with the remembered settings.json
                std::string msg;
                const std::filesystem::path sj = v.empty() ? plat.data_dir() / "harness-rawaccel" / "settings.json" : std::filesystem::path(v);
                const bool ok = app.games_turn_on(sj, &msg);
                app.mouse.patch_message = msg;
                std::printf("games-on: %s %s\n", ok ? "ok" : "failed", msg.c_str());
            } else if (k == "games-off") {
                app.games_turn_off();
            } else if (k == "theme") {  // light | dark | system
                setup.app.theme = v;
                app.save();
            } else if (k == "system-dark") {
                plat.system_dark = v == "1";
            } else if (k == "extras") {
                setup.app.show_extra_tools = v == "1";
                app.save();
            } else if (k == "model") {
                setup.mouse.model = v;
                app.model_input_loaded = false;
                app.save();
            } else if (k == "reminder") {
                setup.app.check_reminder_days = std::atoi(v.c_str());
                app.save();
            } else if (k == "startup") {
                app.set_start_with_windows(v == "1");
            } else if (k == "refresh-switch") {  // "Switch to … Hz" on the first slow monitor
                const auto slow = app.slow_displays();
                if (!slow.empty())
                    fitapp::switch_refresh_rate(app, slow[0].display.device, slow[0].display.refresh_hz, slow[0].advice.max_hz, "");
            } else if (k == "refresh-keep") {  // "Keep … Hz" on the first slow monitor
                const auto slow = app.slow_displays();
                if (!slow.empty()) app.keep_refresh(slow[0].key);
            } else if (k == "print-refresh") {
                const auto all = app.slow_displays(true);
                std::printf("refresh: slow=%zu kept=%d pending=%d hz=%.0f\n", all.size(), !all.empty() && all[0].kept,
                            app.motion.pending.active, app.displays().empty() ? 0.0 : app.displays()[0].refresh_hz);
                for (const auto& c : setup.applied_changes)
                    if (c.kind == "display.refresh_rate") std::printf("refresh change: %s -> %s %s\n", c.before.c_str(), c.after.c_str(), c.status.c_str());
            } else if (k == "setup-reset") {  // as on first run
                setup.app.setup_done = false;
                setup.app.setup_step = v.empty() ? 0 : std::atoi(v.c_str());
                app.save();
            } else if (k == "setup-step") {
                setup.app.setup_step = std::atoi(v.c_str());
            } else if (k == "setup-finish") {
                app.finish_setup();
            } else if (k == "fake-download") {  // url|local file: what a download of that url returns
                const auto parts = split(v, '|');
                if (parts.size() == 2) plat.fake_downloads[parts[0]] = parts[1];
            } else if (k == "vc-runtime") {
                plat.vc_runtime = v == "1";
            } else if (k == "ra-install") {  // "Download and install Raw Accel"
                app.ra_install_start();
            } else if (k == "reboot") {  // the restart after Raw Accel's installer: the driver is there now
                plat.rawaccel_present = true;
            } else if (k == "ra-apply") {  // as if Raw Accel's window was opened and Apply clicked
                namespace fs = std::filesystem;
                const fs::path dir = plat.programs_dir() / "RawAccel";
                std::ofstream(dir / "settings.json") << kSampleRawAccelSettings;
            } else if (k == "print-setup") {
                std::printf("setup: done=%d step=%d ra_step=%d ra_msg=\"%s\" folder=\"%s\" pending=%d driver=%d found=\"%s\"\n",
                            setup.app.setup_done, setup.app.setup_step, int(app.ra_install.step), app.ra_install.message.c_str(),
                            setup.app.rawaccel_folder.c_str(), setup.app.rawaccel_install_pending, plat.rawaccel_present,
                            app.rawaccel_folder().string().c_str());
            } else if (k == "update-check") {
                app.update_check(true);
            } else if (k == "update-download") {
                app.update_download();
            } else if (k == "update-install") {
                app.update_install();
            } else if (k == "print-update") {
                std::printf("update: step=%d tag=%s msg=\"%s\" quit=%d\n", int(app.updater.step),
                            app.updater.release ? app.updater.release->tag.c_str() : "-", app.updater.message.c_str(),
                            plat.quit_requested);
            } else if (k == "startup-blocked") {  // as if switched off in Task Manager → Startup apps
                plat.autostart_blocked_flag = v == "1";
            } else if (k == "game-sens") {  // exe|sensitivity|cm360
                const auto parts = split(v, '|');
                fit::GameSensitivity g;
                if (parts.size() > 1) g.sensitivity = std::atof(parts[1].c_str());
                if (parts.size() > 2) g.cm360 = std::atof(parts[2].c_str());
                setup.app.game_sens[parts[0]] = g;
                app.save();
            } else if (k == "diag") {
                app.diagnostics = app.check_setup();
                for (const auto& d : app.diagnostics)
                    std::printf("diag: %s %s: %s\n", d.ok ? "OK   " : "CHECK", d.what.c_str(), d.detail.c_str());
            } else if (k == "check") {  // the 90-second quick check of the chosen fix
                app.start_check();
                driving = true;
            } else if (k == "age-fix") {  // pretend the chosen fix was last measured N days ago
                if (auto* m = app.active_fix()) {
                    const int64_t t = fit::unix_now() - int64_t(std::atoi(v.c_str())) * 86400;
                    m->created_utc = fit::utc_iso_from_unix(t);
                    if (m->validation) m->validation->finished_utc = m->created_utc;
                    m->last_check.reset();
                }
            } else if (k == "reminder-now") {
                const std::string r = app.take_reminder();
                std::printf("reminder: %s\n", r.empty() ? "(none)" : r.c_str());
            } else if (k == "print-check") {
                if (auto* m = app.active_fix())
                    std::printf("check: %s | %s\n", m->last_check ? m->last_check->verdict.c_str() : "(none)",
                                m->last_check ? m->last_check->explanation.c_str() : "");
                else std::printf("check: no fix chosen\n");
            } else if (k == "confirm-everywhere") {  // as if "Use it everywhere instead…" was clicked
                app.mouse.confirm_everywhere = true;
            } else if (k == "writer-fails") {
                plat.writer_exit_code = std::atoi(v.c_str());
            } else if (k == "print-games") {
                const auto st = app.games_state();
                std::printf("games: enabled=%d fix_on=%s switching=%d fg=%s test_running=%d error=%s autostart=%d keep_running=%d "
                            "tray=\"%s\"\n",
                            st.enabled, !st.fix_on ? "unknown" : *st.fix_on ? "on" : "off", st.switching, st.foreground.c_str(),
                            app.mouse.test ? 1 : 0, st.error.c_str(), plat.autostart, plat.keep_running, app.tray_text().c_str());
            } else if (k == "baseline") {
                app.capture_baseline();
            } else if (k == "focusloss") {
                plat.set_focused(false);
                focus_frames = 3;
            } else if (k == "quit") {
                return false;
            } else {
                std::fprintf(stderr, "unknown command %s\n", k.c_str());
            }
        }
        return !(pc >= cmds.size() && wait == 0 && !driving && pending_shot.empty() && quit);
    }
};

ImGuiKey map_key(KeySym k) {
    switch (k) {
        case XK_Escape: return ImGuiKey_Escape;
        case XK_space: return ImGuiKey_Space;
        case XK_Left: return ImGuiKey_LeftArrow;
        case XK_Right: return ImGuiKey_RightArrow;
        case XK_Up: return ImGuiKey_UpArrow;
        case XK_Down: return ImGuiKey_DownArrow;
        case XK_Return: return ImGuiKey_Enter;
        case XK_BackSpace: return ImGuiKey_Backspace;
        case XK_Tab: return ImGuiKey_Tab;
        case XK_w: return ImGuiKey_W;
        case XK_a: return ImGuiKey_A;
        case XK_s: return ImGuiKey_S;
        case XK_d: return ImGuiKey_D;
        default: return ImGuiKey_None;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path data = std::filesystem::current_path() / "harness-data";
    int W = 1600, H = 1000;
    Runner run;
    run.quit = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--data" && i + 1 < argc) data = argv[++i];
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &W, &H);
        else if (a == "--interactive") run.quit = false;
        else {
            const size_t eq = a.find('=');
            run.cmds.push_back({a.substr(0, eq), eq == std::string::npos ? "" : a.substr(eq + 1)});
        }
    }
    if (!load_gl()) {
        std::fprintf(stderr, "libGL.so.1 not found\n");
        return 1;
    }
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::fprintf(stderr, "no X display (run under Xvfb)\n");
        return 1;
    }
    int attrs[] = {kGLX_RGBA, kGLX_DOUBLEBUFFER, kGLX_RED_SIZE, 8, kGLX_GREEN_SIZE, 8, kGLX_BLUE_SIZE, 8, 0};
    XVisualInfo* vi = gl.ChooseVisual(dpy, DefaultScreen(dpy), attrs);
    if (!vi) {
        std::fprintf(stderr, "no GLX visual\n");
        return 1;
    }
    XSetWindowAttributes swa{};
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     StructureNotifyMask | FocusChangeMask;
    Window win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, unsigned(W), unsigned(H), 0, vi->depth, InputOutput,
                               vi->visual, CWColormap | CWEventMask, &swa);
    XStoreName(dpy, win, "FitSuite (development harness)");
    XMapWindow(dpy, win);
    GLXContext ctx = gl.CreateContext(dpy, vi, nullptr, True);
    gl.MakeCurrent(dpy, win, ctx);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendPlatformName = "fitsuite_x11_harness";
    fitapp::theme::setup(1.0f);
    ImGui_ImplOpenGL3_Init("#version 130");

    fitapp::HarnessPlatform plat(data);
    fitapp::App app(plat);
    app.init();

    bool running = true;
    while (running) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
                case ConfigureNotify: W = ev.xconfigure.width; H = ev.xconfigure.height; break;
                case MotionNotify: io.AddMousePosEvent(float(ev.xmotion.x), float(ev.xmotion.y)); break;
                case ButtonPress:
                case ButtonRelease:
                    if (ev.xbutton.button >= 1 && ev.xbutton.button <= 3)
                        io.AddMouseButtonEvent(ev.xbutton.button == 1 ? 0 : ev.xbutton.button == 3 ? 1 : 2, ev.type == ButtonPress);
                    if (ev.type == ButtonPress && (ev.xbutton.button == 4 || ev.xbutton.button == 5))
                        io.AddMouseWheelEvent(0, ev.xbutton.button == 4 ? 1.0f : -1.0f);
                    break;
                case KeyPress:
                case KeyRelease: {
                    char buf[16];
                    KeySym ks;
                    const int n = XLookupString(&ev.xkey, buf, sizeof buf, &ks, nullptr);
                    const ImGuiKey k = map_key(ks);
                    if (k != ImGuiKey_None) io.AddKeyEvent(k, ev.type == KeyPress);
                    if (ev.type == KeyPress && n > 0 && (unsigned char)buf[0] >= 32) io.AddInputCharactersUTF8(buf);
                    break;
                }
                default: break;
            }
        }
        plat.clock_us += int64_t(plat.frame_dt_ms * 1000.0);
        io.DeltaTime = float(plat.frame_dt_ms / 1000.0);
        io.DisplaySize = ImVec2(float(W), float(H));
        if (!run.pre_frame(app, plat)) running = false;
        if (!plat.window_focused()) app.on_focus_lost();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();
        // While a simulated test runs, only draw the frames that are photographed: software GL is
        // far slower than the simulation, and the test logic does not depend on drawing.
        const bool draw = !run.driving || !run.pending_shot.empty();
        if (draw) {
            gl.Viewport(0, 0, W, H);
            const ImVec4 bg = fitapp::theme::v4(fitapp::theme::kBg1);
            gl.ClearColor(bg.x, bg.y, bg.z, 1.0f);
            gl.Clear(0x4000);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if (!run.pending_shot.empty()) {
                screenshot(W, H, run.pending_shot);
                run.pending_shot.clear();
            }
            gl.SwapBuffers(dpy, win);
        }
        if (run.pc >= run.cmds.size() && !run.driving && run.wait == 0 && run.quit) running = false;
    }
    app.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    return 0;
}
