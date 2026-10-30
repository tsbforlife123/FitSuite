#include "app.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <thread>

#include "fit/game_activation.hpp"
#include "fit/mouse_schedule.hpp"
#include "fit/rawaccel_export.hpp"
#include "fit/timeutil.hpp"
#include "imgui.h"
#include "implot.h"
#include "theme.hpp"

namespace fitapp {

using namespace fit;

App::App(IPlatform& platform) : platform_(platform), store_(std::make_unique<Store>(platform.data_dir())) {}

App::~App() = default;

// Seconds since start, advanced by the frame clock (real time on Windows, virtual time in the harness).
double App::time() const { return ImGui::GetTime(); }

void App::toast(ui::Tone tone, const std::string& text, double seconds) {
    // The same message again refreshes the existing toast instead of stacking a copy.
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const Toast& t) { return t.text == text; }), toasts_.end());
    toasts_.push_back({text, tone, time() + seconds});
    while (toasts_.size() > 3) toasts_.erase(toasts_.begin());
}

void App::save() {
    if (!save_pending_) save_due_ = clock_s() + 1.0;  // at most one write per second while editing
    save_pending_ = true;
}

void App::save_now() {
    save_pending_ = false;
    std::string err;
    if (!store_->save_setup(setup_, &err)) toast(ui::Tone::Bad, "Could not save your setup: " + err, 10.0);
}

void App::init() {
    std::string err;
    if (!store_->ensure_dirs(&err)) load_messages.push_back("The data folder could not be created: " + err);
    LoadOutcome lo = store_->load_setup();
    if (lo.setup) {
        setup_ = *lo.setup;
    } else {
        setup_ = make_default_setup();
    }
    if (lo.recovered_from_backup)
        load_messages.push_back("Your setup file could not be read, so the most recent backup (" + lo.recovered_from +
                                ") was loaded. The unreadable file was kept as " + lo.corrupt_kept_as + ".");
    else if (!lo.errors.empty() && !lo.setup)
        load_messages.push_back("Your setup file could not be read and no usable backup exists. A new setup was started; "
                                "the old file was kept as " + lo.corrupt_kept_as + ".");
    for (const auto& e : lo.errors) load_messages.push_back("  • " + e);

    // Temporary display changes are undone by Windows when FitSuite exits.
    for (auto& c : setup_.applied_changes)
        if (c.temporary && c.status == "applied") {
            c.status = "reverted";
            c.reverted_utc = utc_now_iso();
        }
    refresh_hardware();
    save_now();

    // Background: keep running when the window closes (setting, or needed by the per-game fix), and
    // refresh the startup entry in case FitSuite.exe was moved (left alone if it already points here).
    platform_.set_keep_running(keep_running());
    if (setup_.app.start_with_windows) platform_.set_autostart(true, &err);
    apply_theme();

    // Show the most recent calibration.
    for (auto it = setup_.mouse_sessions.rbegin(); it != setup_.mouse_sessions.rend(); ++it) {
        Session s;
        if (load_mouse_session(*it, s) && s.kind == SessionKind::Calibration && s.complete) {
            mouse.calibration = std::move(s);
            mouse.loaded_from_disk = true;
            analyze_calibration();
            break;
        }
    }
    initialized_ = true;
}

void App::shutdown() {
    if (shut_down_) return;
    shut_down_ = true;
    if (mouse.test && mouse.test->running()) mouse.test->abort("FitSuite was closed.");
    if (motion.test && motion.test->running()) motion.test->abort("FitSuite was closed.");
    mouse.test.reset();
    motion.test.reset();
    // FitSuite stops switching when it exits, so Raw Accel goes back to the player's own settings.
    const auto& g = setup_.fix_games;
    if ((g.enabled && games_on_ != false) || games_restore_needed_) {
        wait_for_writer(3.0);
        std::string err;
        if (!writer_.token && !g.rawaccel_settings.empty() &&
            rawaccel_run_writer(g.rawaccel_settings, "game-off", "", &err, std::filesystem::path(g.rawaccel_settings).parent_path()))
            wait_for_writer(3.0);
    }
    save_now();
}

void App::wait_for_writer(double seconds) {
    // Real time on purpose: the harness clock only advances with frames, and none are drawn here.
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (writer_.token && std::chrono::steady_clock::now() < until) {
        tick_writer();
        if (writer_.token) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool App::test_running() const {
    return (mouse.test && mouse.test->running()) || (motion.test && motion.test->running());
}

void App::on_focus_lost() {
    if (mouse.test) mouse.test->on_focus_lost();
}

void App::refresh_hardware() {
    PcInfo pc = platform_.detect_pc();
    if (!pc.cpu.empty() || !pc.gpus.empty()) setup_.pc = pc;
    refresh_displays();
}

void App::refresh_displays() {
    displays_ = platform_.displays();
    const auto& displays = displays_;
    const DisplayInfo* pick = nullptr;
    for (const auto& d : displays)
        if (d.hosts_window) pick = &d;
    if (!pick)
        for (const auto& d : displays)
            if (d.primary) pick = &d;
    if (!pick && !displays.empty()) pick = &displays.front();
    if (pick) {
        const std::string overdrive = setup_.display.device == pick->device ? setup_.display.overdrive : std::string();
        setup_.display.name = pick->name;
        setup_.display.device = pick->device;
        setup_.display.edid_id = pick->edid_id;
        setup_.display.width = pick->width;
        setup_.display.height = pick->height;
        setup_.display.refresh_hz = pick->refresh_hz;
        setup_.display.modes_hz = pick->modes_hz;
        // Keep an earlier DDC/CI answer for the same monitor; enumeration itself never asks the monitor.
        if (pick->ddcci != "unknown" || setup_.display.device != pick->device) setup_.display.ddcci = pick->ddcci;
        setup_.display.source = platform_.dev_harness() ? "simulated" : "detected";
        setup_.display.overdrive = overdrive;
    }
    if (!setup_.mouse.device_id.empty()) {
        if (auto d = platform_.mouse().device(setup_.mouse.device_id)) {
            setup_.mouse.name = d->name;
            setup_.mouse.vid_pid = d->vid_pid;
            setup_.mouse.source = platform_.dev_harness() ? "simulated" : "detected";
        }
    }
}

void App::record_result(const std::string& tool, const std::string& kind, const std::string& summary,
                        const std::string& origin) {
    ResultRecord r;
    r.id = make_id("res");
    r.tool = tool;
    r.kind = kind;
    r.timestamp_utc = utc_now_iso();
    r.summary = summary;
    r.origin = origin;
    r.conditions = conditions_for(tool, setup_);
    setup_.results.push_back(r);
    if (setup_.results.size() > 300) setup_.results.erase(setup_.results.begin());
}

void App::capture_baseline() {
    setup_.baseline = snapshot_settings(setup_);
    setup_.baseline_captured_utc = utc_now_iso();
    save();
    toast(ui::Tone::Good, "Baseline recorded. You can restore these settings from the History tab.");
}

bool App::load_mouse_session(const std::string& id, Session& out) {
    std::vector<std::string> errs;
    auto s = store_->load_session(id, errs);
    if (!s) return false;
    out = std::move(*s);
    return true;
}

MouseProfileRecord* App::mouse_profile(const std::string& id) {
    for (auto& m : setup_.mouse_profiles)
        if (m.id == id) return &m;
    return nullptr;
}

SessionConditions App::mouse_conditions() const {
    SessionConditions c;
    c.mouse_name = setup_.mouse.name;
    c.mouse_id = setup_.mouse.device_id;
    c.mouse_vid_pid = setup_.mouse.vid_pid;
    c.dpi = setup_.mouse.dpi;
    c.mapping = main_game_mapping(setup_);  // the test turns the view like the player's main game
    c.display_refresh_hz = setup_.display.refresh_hz;
    c.display_width = setup_.display.width;
    c.display_height = setup_.display.height;
    return c;
}

// While the fix switches per game, FitSuite's tests must see the player's own settings: FitSuite in
// front switches the fix off at once, and a test only starts once Raw Accel has confirmed that.
bool App::games_tests_blocked() {
    if (!setup_.fix_games.enabled || (games_on_ == false && !games_pending_)) return false;
    if (!games_error_.empty())
        toast(ui::Tone::Warn, "Switching your fix is paused (Mouse tab, \"Use it in your games\"), so it may still be on in Raw Accel. Sort that "
                              "out first, so the test measures your own settings.", 10.0);
    else
        toast(ui::Tone::Info, "One moment: Raw Accel is switching your fix off so the test measures your own settings. "
                              "Try again in a second.");
    return true;
}

void App::start_calibration() {
    if (!main_game_ready(setup_)) {
        toast(ui::Tone::Warn, "Enter your DPI and your " + main_game_sens_label(setup_) + " first.");
        return;
    }
    if (games_tests_blocked()) return;
    RoundPlanOptions o;
    o.flick_rounds = std::clamp(mouse.calib_blocks, 1, 4);
    o.flick_radius_deg = mouse.radius_deg;
    o.seed = uint64_t(unix_now()) * 2654435761ull;
    MouseTestConfig cfg;
    cfg.kind = SessionKind::Calibration;
    cfg.plan = make_round_calibration_plan(o);
    cfg.conditions = mouse_conditions();
    cfg.locked_device_id = setup_.mouse.device_id;
    cfg.simulated_input = platform_.dev_harness();
    mouse.test = std::make_unique<MouseTest>(platform_, std::move(cfg));
    platform_.set_fullscreen(true);
}

void App::start_validation(const std::string& profile_id) {
    MouseProfileRecord* m = mouse_profile(profile_id);
    if (!m) return;
    if (games_tests_blocked()) return;
    ValidationPlanOptions o;
    o.blocks = mouse.val_blocks;
    o.radius_deg = mouse.radius_deg;
    o.seed = uint64_t(unix_now()) * 0x9E3779B97F4A7C15ull;
    MouseTestConfig cfg;
    cfg.kind = SessionKind::Validation;
    cfg.plan = make_validation_plan(o);
    cfg.candidate = m->transform;
    cfg.candidate_profile_id = m->id;
    cfg.conditions = mouse_conditions();
    cfg.locked_device_id = setup_.mouse.device_id;
    cfg.simulated_input = platform_.dev_harness();
    mouse.selected_profile = profile_id;
    mouse.test = std::make_unique<MouseTest>(platform_, std::move(cfg));
    platform_.set_fullscreen(true);
}

void App::analyze_calibration() {
    if (!mouse.calibration) return;
    const Session& s = *mouse.calibration;
    mouse.calib_metrics.clear();
    for (const auto& t : s.trials)
        if (t.spec.purpose == TrialPurpose::Scored) mouse.calib_metrics.push_back(analyze_trial(t, s.conditions.mapping));
    mouse.fit = fit_profiles(s);
    mouse.aim_report.reset();
    if (!s.rounds.empty()) mouse.aim_report = make_aim_report(s);
    const FitResult& r = *mouse.fit;
    if (!r.ok) return;
    bool exists = false;
    for (const auto& m : setup_.mouse_profiles) exists = exists || m.calibration_session == s.id;
    if (exists) return;
    const std::string date = short_time(s.finished_utc);
    auto add = [&](TransformKind kind, const char* label) {
        MouseProfileRecord m;
        m.id = make_id("mp");
        m.label = std::string(label) + " · " + date;
        m.created_utc = utc_now_iso();
        m.algorithm_version = s.algorithm_version;
        m.calibration_session = s.id;
        m.origin = s.conditions.data_origin;
        m.transform.kind = kind;
        m.transform.directional = r.directional;
        m.transform.raw_accel = r.raw_accel;
        m.conditions = conditions_for("mousefit", setup_);
        setup_.mouse_profiles.push_back(m);
        if (mouse.selected_profile.empty()) mouse.selected_profile = m.id;
    };
    if (r.raw_accel_recommended) add(TransformKind::RawAccel, "Raw Accel-compatible");
    if (r.directional_recommended) add(TransformKind::Directional, "Smooth directional");
}

void App::on_mouse_test_done() {
    platform_.set_fullscreen(false);
    MouseTest& t = *mouse.test;
    const bool aborted = t.aborted();
    const std::string reason = t.abort_reason();
    Session s = t.take_session();
    mouse.test.reset();
    size_t scored = 0;
    for (const auto& tr : s.trials)
        if (tr.spec.purpose == TrialPurpose::Scored && tr.outcome != TrialOutcome::Interrupted) ++scored;
    if (scored == 0) {
        toast(ui::Tone::Info, aborted ? reason + " No data was recorded." : "No targets were completed.");
        return;
    }
    std::string err;
    if (!store_->save_session(s, &err)) toast(ui::Tone::Bad, "The session could not be saved: " + err, 10.0);
    else setup_.mouse_sessions.push_back(s.id);
    if (s.conditions.polling_hz_measured > 0) setup_.mouse.polling_hz = s.conditions.polling_hz_measured;
    if (setup_.mouse.device_id.empty() && !s.conditions.mouse_id.empty()) {
        setup_.mouse.device_id = s.conditions.mouse_id;
        if (auto d = platform_.mouse().device(s.conditions.mouse_id)) {
            setup_.mouse.name = d->name;
            setup_.mouse.vid_pid = d->vid_pid;
            setup_.mouse.source = platform_.dev_harness() ? "simulated" : "detected";
        }
    }

    if (s.kind == SessionKind::Calibration) {
        mouse.calibration = std::move(s);
        mouse.loaded_from_disk = false;
        const size_t before = setup_.mouse_profiles.size();
        analyze_calibration();
        const FitResult& r = *mouse.fit;
        std::string summary = !r.ok ? r.message
                              : (r.directional_recommended || r.raw_accel_recommended)
                                  ? "Directional bias found; candidate correction ready for blinded validation."
                                  : "No correction recommended; directional differences are within noise.";
        if (!mouse.calibration->complete) summary = "Partial calibration (" + reason + "). " + summary;
        record_result("mousefit", "calibration", summary, mouse.calibration->conditions.data_origin);
        mouse.tab = 1;
        toast(r.ok ? ui::Tone::Good : ui::Tone::Warn,
              setup_.mouse_profiles.size() > before ? "Calibration analysed: candidate correction ready."
                                                    : "Calibration analysed.");
        // A fix found by a full calibration goes straight to its blind test (its first screen has Start
        // and Cancel, so the player can still do it later from the Mouse tab).
        if (mouse.calibration->complete && r.ok && r.raw_accel_recommended) {
            std::string fix_id;
            for (const auto& m : setup_.mouse_profiles)
                if (m.calibration_session == mouse.calibration->id && m.transform.kind == TransformKind::RawAccel) fix_id = m.id;
            if (!fix_id.empty()) {
                save();
                start_validation(fix_id);
                if (mouse.test)
                    toast(ui::Tone::Info, "Your flicks need a fix. The blind test now checks that it really makes you faster.", 8.0);
                return;
            }
        }
    } else if (s.kind == SessionKind::Check) {
        // Compare with what the fix's own calibration predicts for flicks with the fix in use.
        DirectionalProfile expected;
        if (MouseProfileRecord* m = mouse_profile(s.candidate_profile_id)) {
            Session cal;
            if (load_mouse_session(m->calibration_session, cal) && cal.kind == SessionKind::Calibration)
                expected = expected_leftover(cal, m->transform);
        }
        const CheckResult cr = check_fix(s, expected);
        CheckSummary c;
        c.session_id = s.id;
        c.finished_utc = s.finished_utc;
        c.verdict = to_string(cr.verdict);
        c.explanation = cr.explanation;
        c.max_residual_pct = cr.max_residual_pct;
        if (!s.complete) {
            c.verdict = to_string(CheckVerdict::NotEnoughData);
            c.explanation = "The check was not finished (" + reason + "). Run it again for an answer.";
        }
        if (MouseProfileRecord* m = mouse_profile(s.candidate_profile_id)) m->last_check = c;
        record_result("mousefit", "check", c.explanation, s.conditions.data_origin);
        toast(c.verdict == "changed" ? ui::Tone::Warn : c.verdict == "still_fits" ? ui::Tone::Good : ui::Tone::Info,
              c.verdict == "changed" ? "Quick check: your aim has changed. Measure again for a new fix."
              : c.verdict == "still_fits" ? "Quick check: your fix still fits."
                                          : "Quick check: not enough data for an answer.",
              8.0);
    } else {
        CompareOptions co;
        mouse.comparison = compare_session(s, co);
        if (!s.complete) {
            mouse.comparison->recommend_candidate = false;
            mouse.comparison->verdict = Verdict::InsufficientData;
            mouse.comparison->explanation =
                "The comparison was not finished (" + reason +
                "), so its counterbalancing is incomplete and it is not used for a recommendation.";
        }
        if (MouseProfileRecord* m = mouse_profile(s.candidate_profile_id)) {
            ValidationSummary v;
            v.session_id = s.id;
            v.finished_utc = s.finished_utc;
            v.verdict = to_string(mouse.comparison->verdict);
            v.explanation = mouse.comparison->explanation;
            v.time_ratio = mouse.comparison->time_ratio;
            v.time_ratio_lo = mouse.comparison->time_ratio_ci.lo;
            v.time_ratio_hi = mouse.comparison->time_ratio_ci.hi;
            v.hit_rate_diff = mouse.comparison->hit_rate_diff;
            v.strength = m->transform.kind == TransformKind::Directional ? m->transform.directional.strength : 1.0;
            m->validation = v;
        }
        record_result("mousefit", "validation", mouse.comparison->explanation, s.conditions.data_origin);
        const bool passed = mouse.comparison->recommend_candidate;
        const std::string fix_id = s.candidate_profile_id;
        mouse.validation = std::move(s);
        mouse.tab = 3;
        // A fix that passed is used straight away: chosen, and switched on in the player's games
        // through Raw Accel when it is installed and its settings.json is known.
        MouseProfileRecord* m = mouse_profile(fix_id);
        if (passed && m && m->transform.kind == TransformKind::RawAccel) {
            setup_.active_mouse_profile = m->id;
            for (auto& x : setup_.mouse_profiles)
                if (x.status == "active") x.status = "candidate";
            m->status = "active";
            // At least the main game, so the fix has somewhere to switch on.
            if (setup_.fix_games.games.empty()) setup_.fix_games.games.push_back(main_game_exe(setup_.game.id));
            save_now();
            std::string msg, made;
            const std::filesystem::path sj = rawaccel_installed() ? rawaccel_settings_ready(&made) : std::filesystem::path();
            if (!rawaccel_installed())
                toast(ui::Tone::Good, "Your fix passed. Install Raw Accel (Settings → About → Run the setup again) to use it in your games.", 10.0);
            else if (sj.empty())
                toast(ui::Tone::Warn, "Your fix passed, but Raw Accel's folder was not found. Click \"Turn on\" on the Mouse tab "
                                      "and choose settings.json in your Raw Accel folder.", 12.0);
            else if (games_turn_on(sj, &msg))
                toast(ui::Tone::Good, "Your fix passed and is now on in your games.", 10.0);
            else
                toast(ui::Tone::Warn, "Your fix passed, but it could not be switched on: " + msg, 10.0);
            mouse.patch_message = made.empty() ? msg : msg.empty() ? made : made + " " + msg;
        }
    }
    save();
}

MouseProfileRecord* App::active_fix() {
    MouseProfileRecord* m = mouse_profile(setup_.active_mouse_profile);
    return m && m->transform.kind == TransformKind::RawAccel ? m : nullptr;
}

void App::start_check() {
    MouseProfileRecord* m = active_fix();
    if (!m) {
        toast(ui::Tone::Info, "Choose a fix first: it has to pass the blind test.");
        return;
    }
    if (games_tests_blocked()) return;
    // When the fix is the Raw Accel default, Raw Accel already applies it to the mouse input FitSuite
    // reads; otherwise FitSuite applies it during the check (Raw Accel is on your own settings then).
    bool everywhere = false;
    for (const auto& c : setup_.applied_changes)
        if ((c.kind == "mousefit.rawaccel_default" || c.kind == "mousefit.rawaccel_apply") && c.status == "applied")
            everywhere = true;
    CheckPlanOptions o;
    o.radius_deg = mouse.radius_deg;
    o.seed = uint64_t(unix_now()) * 0xD1B54A32D192ED03ull;
    o.condition = everywhere ? Condition::Baseline : Condition::Candidate;
    MouseTestConfig cfg;
    cfg.kind = SessionKind::Check;
    cfg.plan = make_check_plan(o);
    cfg.candidate = m->transform;
    cfg.candidate_profile_id = m->id;
    cfg.conditions = mouse_conditions();
    cfg.locked_device_id = setup_.mouse.device_id;
    cfg.simulated_input = platform_.dev_harness();
    mouse.test = std::make_unique<MouseTest>(platform_, std::move(cfg));
    platform_.set_fullscreen(true);
}

int App::days_since_fix_checked() {
    const MouseProfileRecord* m = active_fix();
    if (!m) return -1;
    int64_t latest = 0, t = 0;
    if (parse_utc_iso(m->created_utc, t)) latest = std::max(latest, t);
    if (m->validation && parse_utc_iso(m->validation->finished_utc, t)) latest = std::max(latest, t);
    if (m->last_check && m->last_check->verdict != "not_enough_data" && parse_utc_iso(m->last_check->finished_utc, t))
        latest = std::max(latest, t);
    if (!latest) return -1;
    return int(std::max<int64_t>(0, (unix_now() - latest) / 86400));
}

bool App::check_due() {
    const int every = setup_.app.check_reminder_days;
    if (every <= 0) return false;
    const int d = days_since_fix_checked();
    return d >= every;
}

void App::remove_fix() {
    if (setup_.fix_games.enabled) games_turn_off();
    for (auto& c : setup_.applied_changes)
        if ((c.kind == "mousefit.rawaccel_default" || c.kind == "mousefit.rawaccel_apply") && c.status == "applied") {
            std::string err;
            if (!rawaccel_restore(c, &err)) toast(ui::Tone::Bad, err, 10.0);
            break;  // one writer at a time; the newest applied change is the one in use
        }
    if (MouseProfileRecord* m = mouse_profile(setup_.active_mouse_profile)) m->status = "candidate";
    setup_.active_mouse_profile.clear();
    save_now();
    toast(ui::Tone::Info, "The fix is no longer used. Your own Raw Accel settings are back.");
}

std::string App::take_reminder() {
    if (!check_due() || games_on_ == true) return "";
    int64_t last = 0;
    if (parse_utc_iso(setup_.app.last_reminder_utc, last) && unix_now() - last < 86400) return "";
    setup_.app.last_reminder_utc = utc_now_iso();
    save();
    return ui::fmt("Your mouse fix was last checked %d days ago. Open FitSuite for a one-minute check.",
                   days_since_fix_checked());
}

bool App::set_start_with_windows(bool on) {
    std::string err;
    if (!platform_.set_autostart(on, &err)) {
        toast(ui::Tone::Bad, err, 10.0);
        return false;
    }
    if (on) platform_.unblock_autostart();  // the player asked for it, so also undo a "Disable" in Task Manager
    autostart_blocked_at_ = -1.0;
    const bool was = setup_.app.start_with_windows;
    setup_.app.start_with_windows = on;
    // The startup entry is a change to Windows, so it is listed under History with its way back.
    if (on && !was) {
        AppliedChange ch;
        ch.id = make_id("ch");
        ch.kind = "app.startup_entry";
        ch.target = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run, value FitSuite";
        ch.before = "no entry";
        ch.after = "FitSuite.exe --autostart";
        ch.applied_utc = utc_now_iso();
        ch.status = "applied";
        setup_.applied_changes.push_back(std::move(ch));
    } else if (!on) {
        for (auto& c : setup_.applied_changes)
            if (c.kind == "app.startup_entry" && c.status == "applied") {
                c.status = "reverted";
                c.reverted_utc = utc_now_iso();
            }
    }
    save();
    return true;
}

bool App::startup_blocked() {
    if (!setup_.app.start_with_windows) return false;
    const double now = clock_s();
    if (autostart_blocked_at_ < 0.0 || now - autostart_blocked_at_ > 3.0) {
        autostart_blocked_ = platform_.autostart_blocked();
        autostart_blocked_at_ = now;
    }
    return autostart_blocked_;
}

std::filesystem::path App::rawaccel_settings_path() {
    std::string err;
    if (auto t = read_file(platform_.data_dir() / "rawaccel-settings-path.txt", &err, 4096)) {
        std::string p = *t;
        while (!p.empty() && (p.back() == '\n' || p.back() == '\r' || p.back() == ' ')) p.pop_back();
        std::error_code ec;
        if (!p.empty() && std::filesystem::exists(std::filesystem::path(p), ec)) return std::filesystem::path(p);
    }
    if (!setup_.fix_games.rawaccel_settings.empty()) return setup_.fix_games.rawaccel_settings;
    return {};
}

void App::remember_rawaccel_settings_path(const std::filesystem::path& p) {
    std::string err;
    atomic_write_file(platform_.data_dir() / "rawaccel-settings-path.txt", p.string(), &err);
}

std::string App::display_id(const DisplayInfo& d) {
    return !d.edid_id.empty() ? d.edid_id : !d.name.empty() ? d.name : d.device;
}

std::vector<App::SlowDisplay> App::slow_displays(bool include_kept) const {
    std::vector<SlowDisplay> out;
    for (const auto& d : displays_) {
        const fit::RefreshAdvice a = fit::refresh_advice(d.refresh_hz, d.modes_hz);
        if (!a.slower) continue;
        const std::string key = fit::refresh_dismiss_key(display_id(d), a.max_hz);
        const auto& kept = setup_.app.refresh_kept;
        const bool is_kept = std::find(kept.begin(), kept.end(), key) != kept.end();
        if (is_kept && !include_kept) continue;
        out.push_back({d, a, key, is_kept});
    }
    return out;
}

void App::keep_refresh(const std::string& key) {
    auto& kept = setup_.app.refresh_kept;
    if (std::find(kept.begin(), kept.end(), key) == kept.end()) kept.push_back(key);
    save();
}

std::vector<App::DiagItem> App::check_setup() {
    namespace fs = std::filesystem;
    std::vector<DiagItem> out;
    auto add = [&](bool ok, const std::string& what, const std::string& detail) { out.push_back({ok, what, detail}); };
    const bool driver = platform_.rawaccel_driver_present();
    rawaccel_installed_ = driver;
    rawaccel_checked_at_ = clock_s();
    add(driver, "Raw Accel driver", driver ? "Installed." : "Not installed. It is needed to use your fix in games.");
    const fs::path sj = driver ? rawaccel_settings_ready() : rawaccel_settings_path();
    std::error_code ec;
    if (sj.empty()) {
        add(false, "Raw Accel settings.json", "Raw Accel's folder was not found. FitSuite asks for it the first time you turn the fix on.");
    } else {
        std::string err;
        auto text = read_file(sj, &err, 8u << 20);
        const bool readable = text && patch_rawaccel_settings(*text, RawAccelProfile{}, "", &err).has_value();
        add(readable, "Raw Accel settings.json", readable ? sj.string() : sj.string() + ": " + err);
        const bool writer = fs::exists(sj.parent_path() / "writer.exe", ec);
        add(writer, "Raw Accel writer.exe", writer ? "Found next to settings.json." : "Not found next to settings.json.");
    }
    refresh_displays();
    for (const auto& d : displays_) {
        const fit::RefreshAdvice a = fit::refresh_advice(d.refresh_hz, d.modes_hz);
        const std::string name = d.name.empty() ? d.device : d.name;
        const std::string what = displays_.size() > 1 ? "Refresh rate: " + name : std::string("Monitor refresh rate");
        if (d.refresh_hz <= 0.0) {
            add(true, what, "Not reported by Windows.");
        } else if (!a.slower) {
            add(true, what,
                ui::fmt("%.0f Hz, the fastest Windows offers for %s at %dx%d. If your monitor is sold as faster, check "
                        "the cable and port (DisplayPort, or the cable that came with it).",
                        d.refresh_hz, name.c_str(), d.width, d.height));
        } else {
            const auto& kept = setup_.app.refresh_kept;
            const bool is_kept = std::find(kept.begin(), kept.end(), fit::refresh_dismiss_key(display_id(d), a.max_hz)) != kept.end();
            add(is_kept, what,
                is_kept ? ui::fmt("%.0f Hz by your choice; up to %.0f Hz is available.", d.refresh_hz, a.max_hz)
                        : ui::fmt("%.0f Hz, but this monitor can run at %.0f Hz. Switch it on the Mouse tab.", d.refresh_hz,
                                  a.max_hz));
        }
    }
    const auto mice = platform_.mouse().devices();
    add(!mice.empty(), "Mouse", mice.empty() ? "No mouse found." : ui::fmt("%zu found", mice.size()) +
                                                                       (setup_.mouse.model.empty() ? "" : "; your model: " + setup_.mouse.model));
    if (setup_.app.start_with_windows) {
        const bool entry = platform_.autostart_enabled();
        autostart_blocked_at_ = -1.0;
        const bool blocked = entry && startup_blocked();
        add(entry && !blocked, "Open when Windows starts",
            !entry    ? "On in Settings, but the startup entry is missing. Turn the setting off and on again."
            : blocked ? "Switched off in Task Manager → Startup apps, so Windows does not start FitSuite. Untick and tick "
                        "\"Open FitSuite when Windows starts\" to switch it back on."
                      : "On.");
    } else if (setup_.fix_games.enabled) {
        // Raw Accel's driver keeps no settings over a restart, and only FitSuite switches the fix.
        add(false, "Open when Windows starts",
            "Off. After a restart your fix stays off in your games until you open FitSuite. Tick \"Open FitSuite when "
            "Windows starts\" under Startup and background.");
    } else {
        add(true, "Open when Windows starts", "Off.");
    }
    if (setup_.fix_games.enabled) {
        const GamesState gs = games_state();
        add(gs.error.empty() && gs.have_fix, "Fix in your games",
            !gs.error.empty() ? gs.error : !gs.have_fix ? "On, but no fix is chosen." : "On, switching by the game in front.");
    }
    {
        const fs::path probe = platform_.data_dir() / "write-test.tmp";
        std::string err;
        const bool ok = atomic_write_file(probe, "ok", &err);
        fs::remove(probe, ec);
        add(ok, "Data folder", ok ? platform_.data_dir().string() : platform_.data_dir().string() + ": " + err);
    }
    if (setup_.app.show_extra_tools) {
        const PresentMonStatus pm = platform_.presentmon().status();
        add(pm.available, "PresentMon (Frame rate tab)", pm.available ? pm.path : pm.note);
    }
    diagnostics_time = utc_now_iso();
    return out;
}

void App::start_motion_test(MotionTestConfig cfg) {
    cfg.simulated_input = platform_.dev_harness();
    motion.test = std::make_unique<MotionTest>(platform_, std::move(cfg));
    platform_.set_fullscreen(true);
}

void App::on_motion_test_done() {
    platform_.set_fullscreen(false);
    MotionTest& t = *motion.test;
    const bool aborted = t.aborted();
    const std::string reason = t.abort_reason();
    MotionSessionRecord rec = t.take_record();
    motion.test.reset();
    if (rec.responses.empty() && rec.ratings.empty()) {
        toast(ui::Tone::Info, aborted ? reason + " No data was recorded." : "No answers were recorded.");
        return;
    }
    rec.conditions = conditions_for("motionfit", setup_);
    std::string summary;
    if (rec.test == "identification" && rec.configs.size() >= 2) {
        const auto c = compare_motion(rec.responses, 0, 1);
        summary = c.explanation;
    } else {
        summary = ui::fmt("%zu answers recorded.", rec.responses.size() + rec.ratings.size());
    }
    if (aborted) summary = "Stopped early (" + reason + "). " + summary;
    record_result("motionfit", rec.test, summary, rec.origin);
    motion.last_session = rec.id;
    setup_.motion_sessions.push_back(std::move(rec));
    save();
    toast(ui::Tone::Good, "Motion test saved.");
}

// ---- frame --------------------------------------------------------------------------------------

// Classic tabs along the top, like a Windows settings window.
// Classic tabs along the top. Frame rate and Monitor only show when the extra tools are switched on
// in Settings: FitSuite is first of all the mouse fix.
void App::draw_tabs() {
    using namespace theme;
    const bool extras = setup_.app.show_extra_tools;
    if (!extras && (page_ == Page::FrameFit || page_ == Page::MotionFit)) page_ = Page::MouseFit;
    std::vector<const char*> labels = {"Mouse"};
    std::vector<Page> pages = {Page::MouseFit};
    if (extras) {
        labels.insert(labels.end(), {"Frame rate", "Monitor"});
        pages.insert(pages.end(), {Page::FrameFit, Page::MotionFit});
    }
    labels.insert(labels.end(), {"History", "Nerd stats", "Settings"});
    pages.insert(pages.end(), {Page::Profiles, Page::Nerd, Page::Settings});
    int current = 0;
    for (size_t i = 0; i < pages.size(); ++i)
        if (pages[i] == page_) current = int(i);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetTextLineHeight() + 10.0f + 8.0f;
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + h), rgb(kBg0));
    ImGui::SetCursorScreenPos(ImVec2(p.x + 8, p.y + 8));
    const int clicked = ui::tabs(labels.data(), int(labels.size()), current);
    if (clicked >= 0) page_ = pages[size_t(clicked)];
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
}

void App::draw_content(ImVec2 spacing) {
    const float status_h = ImGui::GetTextLineHeight() + 10.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::v4(theme::kBg1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
    ImGui::BeginChild("##content", ImVec2(0, -status_h), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, spacing);
    if (!load_messages.empty()) {
        std::string all;
        for (const auto& m : load_messages) all += m + "\n";
        ui::callout(ui::Tone::Warn, "Your saved data needed attention", all.c_str());
        if (ui::button_secondary("OK")) load_messages.clear();
    }
    if (!setup_.app.setup_done) {
        page_setup(*this);  // first run: the setup comes before everything else
    } else switch (page_) {
        case Page::Overview:
        case Page::MouseFit: page_mousefit(*this); break;
        case Page::FrameFit: page_framefit(*this); break;
        case Page::MotionFit: page_motionfit(*this); break;
        case Page::Profiles: page_profiles(*this); break;
        case Page::Nerd: page_nerd(*this); break;
        case Page::Settings: page_settings(*this); break;
    }
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// One line at the bottom: the mouse fix, Raw Accel, the latest message, and what build this is.
void App::draw_status_bar() {
    using namespace theme;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetTextLineHeight() + 10.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), rgb(kBg0));
    dl->AddLine(p, ImVec2(p.x + w, p.y), rgb(kBorder));
    ImGui::SetCursorScreenPos(ImVec2(p.x + 10, p.y + 5));

    const GamesState gs = games_state();
    bool everywhere = false;
    for (const auto& c : setup_.applied_changes)
        if ((c.kind == "mousefit.rawaccel_apply" || c.kind == "mousefit.rawaccel_default") && c.status == "applied") everywhere = true;
    std::string fix = "Mouse fix: not set up";
    unsigned fix_color = kTextMuted;
    if (gs.enabled) {
        if (!gs.error.empty()) fix = "Mouse fix: paused", fix_color = kWarn;
        else if (!gs.have_fix) fix = "Mouse fix: no fix chosen", fix_color = kWarn;
        else if (gs.fix_on == true) fix = "Mouse fix: on in " + game_display_name(gs.game), fix_color = kGood;
        else fix = "Mouse fix: ready, off outside your games", fix_color = kText;
    } else if (everywhere) {
        fix = "Mouse fix: on everywhere", fix_color = kGood;
    } else if (!setup_.active_mouse_profile.empty()) {
        fix = "Mouse fix: chosen, not in use";
    }
    bool first = true;
    auto separator = [&]() {  // thin divider between status fields
        ImGui::SameLine(0, 10);
        const ImVec2 q = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(q.x, p.y + 4), ImVec2(q.x, p.y + h - 4), rgb(kBorder));
        ImGui::SameLine(0, 11);
    };
    auto field = [&](const std::string& text, unsigned color) {
        if (!first) separator();
        first = false;
        ImGui::PushStyleColor(ImGuiCol_Text, v4(color));
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopStyleColor();
    };
    field(fix, fix_color);
    field(rawaccel_installed() ? "Raw Accel: installed" : "Raw Accel: not installed", kTextMuted);
    if ((updater.step == Updater::Step::Available || updater.step == Updater::Step::Ready) && updater.release)
        field(("Version " + updater.release->version_text() +
               (updater.step == Updater::Step::Ready && setup_.app.auto_install_updates ? " installs when FitSuite next opens"
                                                                                        : " available (Settings)"))
                  .c_str(),
              kAccent);

    // The newest message, if it has not expired.
    const double now = time();
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const Toast& t) { return t.until < now; }), toasts_.end());
    const std::string right_text = platform_.dev_harness() ? std::string("DEVELOPMENT HARNESS · simulated input")
                                                            : std::string("Local only");
    const char* right = right_text.c_str();
    // "What's new" sits just left of the version, and reopens this version's notes.
    const char* notes_link = "What's new";
    const bool has_notes = *kReleaseNotes != '\0';
    const float link_w = has_notes ? ImGui::CalcTextSize(notes_link).x + 21.0f : 0.0f;  // + divider
    const float right_w = ImGui::CalcTextSize(right).x + link_w;
    if (!toasts_.empty()) {
        const Toast& t = toasts_.back();
        unsigned c = kText;
        if (t.tone == ui::Tone::Good) c = kGood;
        if (t.tone == ui::Tone::Warn) c = kWarn;
        if (t.tone == ui::Tone::Bad) c = kBad;
        separator();
        const float x0 = ImGui::GetCursorScreenPos().x;
        const float x1 = p.x + w - right_w - 24.0f;
        if (x1 > x0 + 40) {
            ImGui::PushClipRect(ImVec2(x0, p.y), ImVec2(x1, p.y + h), true);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(c));
            ImGui::TextUnformatted(t.text.c_str());
            ImGui::PopStyleColor();
            ImGui::PopClipRect();
            if (ImGui::IsItemHovered() && ImGui::CalcTextSize(t.text.c_str()).x > x1 - x0) ImGui::SetItemTooltip("%s", t.text.c_str());
            ImGui::SameLine();
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - right_w - 10, p.y + 5));
    if (has_notes) {
        if (ui::button_link(notes_link)) whats_new_open_ = true;
        separator();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, v4(platform_.dev_harness() ? kBad : kTextFaint));
    ImGui::TextUnformatted(right);
    ImGui::PopStyleColor();
}

bool App::rawaccel_installed() {
    const double now = clock_s();
    if (rawaccel_checked_at_ == 0.0 || now - rawaccel_checked_at_ > 3.0) {
        rawaccel_installed_ = platform_.rawaccel_driver_present();
        rawaccel_checked_at_ = now;
    }
    return rawaccel_installed_;
}

void App::draw_test_overlay() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##test", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    if (mouse.test) mouse.test->frame(vp->Pos, vp->Size);
    else if (motion.test) motion.test->frame(vp->Pos, vp->Size);
    ImGui::End();
    ImGui::PopStyleVar(2);
    if (mouse.test && !mouse.test->running()) on_mouse_test_done();
    if (motion.test && !motion.test->running()) on_motion_test_done();
}

bool App::rawaccel_run_writer(const std::filesystem::path& settings, const std::string& purpose, const std::string& change_id,
                              std::string* error, const std::filesystem::path& writer_dir) {
    if (writer_.token) {
        if (error) *error = "Raw Accel's writer is still running.";
        return false;
    }
    const std::filesystem::path writer = (writer_dir.empty() ? settings.parent_path() : writer_dir) / "writer.exe";
    std::error_code ec;
    if (!std::filesystem::exists(writer, ec)) {
        if (error) *error = "writer.exe was not found in " + writer.parent_path().filename().string() + ". Choose the settings.json in "
                            "your Raw Accel folder, or load the file in Raw Accel yourself.";
        return false;
    }
    const uint64_t t = platform_.start_program(writer, {settings}, error);
    if (!t) return false;
    writer_ = {t, purpose, change_id, clock_s()};
    return true;
}

bool App::rawaccel_make_default(const std::filesystem::path& settings_json, const std::filesystem::path& corrected,
                                std::string* message) {
    namespace fs = std::filesystem;
    const fs::path backup = settings_json.parent_path() / ("settings.before-fitsuite-" + utc_compact_now() + ".json");
    std::error_code ec;
    fs::copy_file(settings_json, backup, fs::copy_options::none, ec);
    if (ec) {
        if (message) *message = "Could not save a copy of settings.json (" + ec.message() + "). Nothing was changed.";
        return false;
    }
    std::string err;
    auto text = read_file(corrected, &err, 8u << 20);
    if (!text || !atomic_write_file(settings_json, *text, &err)) {
        if (message) *message = "Could not update settings.json: " + err + " Your settings are unchanged.";
        return false;
    }
    AppliedChange ch;
    ch.id = make_id("ch");
    ch.kind = "mousefit.rawaccel_default";
    ch.target = settings_json.string();
    ch.before = backup.string();
    ch.after = corrected.string();
    ch.applied_utc = utc_now_iso();
    ch.status = "applied";
    setup_.applied_changes.push_back(ch);
    save_now();  // the change log must know about the replaced settings.json before anything else happens
    if (!rawaccel_run_writer(settings_json, "default", ch.id, &err)) {
        if (message) *message = "settings.json now holds the correction (your previous file: " + backup.filename().string() +
                                "). It takes effect when Raw Accel next loads settings.json. " + err;
        return true;
    }
    if (message) *message = "Saved your previous settings as " + backup.filename().string() + " and applied the correction.";
    return true;
}

bool App::rawaccel_restore(AppliedChange& c, std::string* error) {
    if (c.kind == "mousefit.rawaccel_default") {
        auto text = read_file(c.before, error, 8u << 20);
        if (!text) return false;
        if (!atomic_write_file(c.target, *text, error)) return false;
        return rawaccel_run_writer(c.target, "restore", c.id, error);
    }
    if (c.kind == "mousefit.rawaccel_apply") return rawaccel_run_writer(c.before, "restore", c.id, error);
    if (error) *error = "This change cannot be undone automatically.";
    return false;
}

void App::tick_writer() {
    if (!writer_.token) return;
    const auto code = platform_.program_exit_code(writer_.token);
    if (!code) return;  // still running
    AppliedChange* ch = nullptr;
    for (auto& c : setup_.applied_changes)
        if (c.id == writer_.change_id) ch = &c;
    const bool ok = *code == 0;
    if (writer_.purpose == "game-on" || writer_.purpose == "game-off") {
        // Per-game switching: no toasts (the player is in a game); the MouseFit page shows the state.
        if (ok) {
            games_on_ = writer_.purpose == "game-on";
        } else {
            games_on_.reset();
            if (setup_.fix_games.enabled)
                games_error_ = ui::fmt("Raw Accel's writer reported a problem (exit code %d); its own window explains why.",
                                       *code);
            else
                toast(ui::Tone::Bad, ui::fmt("Raw Accel's writer reported a problem (exit code %d) while going back to your own "
                                             "settings. Open Raw Accel to check which settings are active.", *code), 12.0);
        }
        games_pending_.reset();
        writer_ = {};
        return;
    }
    if (writer_.purpose == "default") {
        // settings.json was already replaced; only activating it now may have failed.
        if (ok) toast(ui::Tone::Good, "Raw Accel now uses the FitSuite correction, also after a restart.", 8.0);
        else toast(ui::Tone::Warn, ui::fmt("settings.json now holds the correction, but Raw Accel's writer could not apply it "
                                           "right now (exit code %d). It takes effect when Raw Accel next loads settings.json.",
                                           *code), 12.0);
    } else if (writer_.purpose == "apply") {
        if (ch) ch->status = ok ? "applied" : "failed";
        if (ok) toast(ui::Tone::Good, "Raw Accel now uses the FitSuite correction.", 8.0);
        else toast(ui::Tone::Bad, ui::fmt("Raw Accel's writer reported a problem (exit code %d); its own window explains why. "
                                          "Your mouse settings did not change.", *code), 12.0);
    } else {
        if (ok && ch) {
            ch->status = "reverted";
            ch->reverted_utc = utc_now_iso();
        }
        if (ok) toast(ui::Tone::Good, "Raw Accel is back on your previous settings.", 8.0);
        else toast(ui::Tone::Bad, ui::fmt("Raw Accel's writer reported a problem (exit code %d) while restoring; its own window "
                                          "explains why. Open Raw Accel to check which settings are active.", *code), 12.0);
    }
    writer_ = {};
    save();
}

void App::apply_theme() {
    const std::string& t = setup_.app.theme;
    const bool dark = t == "dark" || (t == "system" && platform_.system_dark_mode().value_or(false));
    if (theme_set_ && dark == theme_dark_) return;
    theme_set_ = true;
    theme_dark_ = dark;
    theme::apply(dark);
    platform_.set_dark_title_bar(dark);
}

void App::frame() {
    tick_writer();
    framefit_tick(*this);
    if (!initialized_) init();
    apply_theme();
    platform_.set_keep_running(keep_running());
    tick_games();
    tick_live();
    tick_ra_install();
    tick_updater();
    if (save_pending_ && clock_s() >= save_due_) save_now();
    if (mouse.test || motion.test) {
        draw_test_overlay();
        return;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));  // tabs, page and status bar touch
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (setup_.app.setup_done) draw_tabs();
    draw_content(spacing);
    draw_status_bar();
    ImGui::End();
    ImGui::PopStyleVar(4);
    tick_pending_display(*this);
    draw_whats_new();
    draw_update_prompt();
}

// ---- MouseFit only in your games ------------------------------------------------------------------

// The player's settings.json with the chosen fix composed into it, in FitSuite's data folder. Made
// again before every switch on, so later changes in Raw Accel or a newly chosen fix are picked up.
bool App::games_write_corrected(std::string* error, std::vector<std::string>* warnings) {
    const auto& g = setup_.fix_games;
    const MouseProfileRecord* m = mouse_profile(setup_.active_mouse_profile);
    if (!m || m->transform.kind != TransformKind::RawAccel) {
        if (error) *error = "No Raw Accel fix is chosen: a fix has to pass the blind test first.";
        return false;
    }
    auto text = read_file(g.rawaccel_settings, error, 8u << 20);
    if (!text) return false;
    auto r = patch_rawaccel_settings(*text, m->transform.raw_accel, "", error);
    if (!r) return false;
    if (warnings) *warnings = r->warnings;
    std::string ignored;
    if (auto existing = read_file(g.rawaccel_corrected, &ignored, 8u << 20); existing && *existing == r->json) return true;
    return atomic_write_file(g.rawaccel_corrected, r->json, error);
}

bool App::games_turn_on(const std::filesystem::path& settings_json, std::string* message) {
    namespace fs = std::filesystem;
    auto& g = setup_.fix_games;
    auto fail = [&](const std::string& m) {
        if (message) *message = m;
        return false;
    };
    const MouseProfileRecord* m = mouse_profile(setup_.active_mouse_profile);
    if (!m || m->transform.kind != TransformKind::RawAccel) return fail("Choose a fix first: it has to pass the blind test.");
    if (g.games.empty()) return fail("Tick at least one game.");
    if (writer_.token) return fail("Raw Accel is still applying a change. Try again in a moment.");
    std::error_code ec;
    if (!fs::exists(settings_json.parent_path() / "writer.exe", ec))
        return fail("writer.exe was not found next to " + settings_json.filename().string() +
                    ". Choose the settings.json in your Raw Accel folder.");

    // In this mode settings.json always holds the player's own settings. If the fix was made the Raw
    // Accel default earlier, the saved copy goes back first.
    std::string note, err;
    for (auto& c : setup_.applied_changes) {
        if (c.status != "applied") continue;
        if (c.kind == "mousefit.rawaccel_default") {
            auto text = read_file(c.before, &err, 8u << 20);
            if (!text || !atomic_write_file(c.target, *text, &err))
                return fail("Could not put your own settings.json back (" + err + "). Nothing was changed.");
            c.status = "reverted";
            c.reverted_utc = utc_now_iso();
            note = " Your settings.json holds your own settings again (the fix was your Raw Accel default before).";
        } else if (c.kind == "mousefit.rawaccel_apply") {
            c.status = "reverted";  // superseded: switching per game decides from now on
            c.reverted_utc = utc_now_iso();
        }
    }
    const GameActivation before = g;
    g.rawaccel_settings = settings_json.string();
    g.rawaccel_corrected = (platform_.data_dir() / "rawaccel-with-fix.json").string();
    std::vector<std::string> warnings;
    if (!games_write_corrected(&err, &warnings)) {
        g = before;
        save();
        return fail("Could not make the copy of your Raw Accel settings with the fix: " + err);
    }
    g.enabled = true;
    platform_.set_keep_running(true);
    if (!setup_.app.start_with_windows)
        note += " Tip: tick \"Open FitSuite when Windows starts\" so the fix keeps switching after a restart.";

    AppliedChange ch;
    ch.id = make_id("ch");
    ch.kind = "mousefit.rawaccel_games";
    ch.target = g.rawaccel_settings;
    ch.before = g.rawaccel_settings;
    ch.after = g.rawaccel_corrected;
    ch.applied_utc = utc_now_iso();
    ch.status = "applied";
    setup_.applied_changes.push_back(ch);

    switcher_.reset();
    games_on_.reset();  // unknown: the first decision is made at once
    games_error_.clear();
    games_restore_needed_ = false;
    games_next_check_ = 0.0;
    save_now();
    mouse.patch_warnings = warnings;
    if (message) {  // the state itself is shown on the page; only report what else happened
        *message = note;
        while (!message->empty() && message->front() == ' ') message->erase(message->begin());
    }
    return true;
}

void App::games_turn_off() {
    auto& g = setup_.fix_games;
    if (!g.enabled) return;
    g.enabled = false;
    platform_.set_keep_running(keep_running());
    for (auto& c : setup_.applied_changes)
        if (c.kind == "mousefit.rawaccel_games" && c.status == "applied") {
            c.status = "reverted";
            c.reverted_utc = utc_now_iso();
        }
    games_restore_needed_ = games_on_ != false || games_pending_ == true;
    switcher_.reset();
    games_error_.clear();
    games_next_check_ = 0.0;
    save_now();
}

void App::games_retry() {
    games_error_.clear();
    switcher_.reset();
    games_on_.reset();
    games_next_check_ = 0.0;
}

void App::tick_games() {
    const auto& g = setup_.fix_games;
    const double now = clock_s();
    if (now < games_next_check_) return;
    games_next_check_ = now + 0.1;
    if (writer_.token) {  // one writer at a time
        // A writer that stays open is most likely showing an error message: stop switching until the player looks.
        if ((writer_.purpose == "game-on" || writer_.purpose == "game-off") && now - writer_.started > 10.0 &&
            games_error_.empty() && g.enabled)
            games_error_ = "Raw Accel's writer has been open for more than 10 seconds; it is probably showing a message. "
                           "Look for its window on the taskbar.";
        return;
    }
    std::string err;
    const std::filesystem::path writer_dir = std::filesystem::path(g.rawaccel_settings).parent_path();
    if (!g.enabled) {
        if (games_restore_needed_) {  // turned off while the fix was (or may have been) on
            games_restore_needed_ = false;
            if (!g.rawaccel_settings.empty() && rawaccel_run_writer(g.rawaccel_settings, "game-off", "", &err, writer_dir))
                games_pending_ = false;
            else
                toast(ui::Tone::Bad, "Could not put Raw Accel back on your own settings: " + err, 12.0);
        }
        return;
    }
    if (!games_error_.empty()) return;  // paused until the player retries

    const std::string fg = platform_.foreground_app();
    if (!fg.empty()) games_fg_ = fit::normalize_exe(fg);
    const MouseProfileRecord* m = mouse_profile(setup_.active_mouse_profile);
    static const std::vector<std::string> no_games;
    const bool have_fix = m && m->transform.kind == TransformKind::RawAccel;
    const auto d = switcher_.update(fg, have_fix ? g.games : no_games, now, games_on_);
    if (!d) return;
    if (*d && !games_write_corrected(&err)) {
        games_error_ = "The copy of your Raw Accel settings with the fix could not be made: " + err;
        return;
    }
    if (!rawaccel_run_writer(*d ? g.rawaccel_corrected : g.rawaccel_settings, *d ? "game-on" : "game-off", "", &err, writer_dir)) {
        games_error_ = err;
        return;
    }
    games_pending_ = *d;
    if (*d) games_on_for_ = games_fg_;
}

// ---- live tuning ----------------------------------------------------------------------------------

namespace {

// Degrees the game in front turns per mouse count, for telling flicks from small adjustments.
double game_deg_per_count(const SetupProfile& s, const std::string& exe) {
    const double yaw = game_yaw(exe);
    if (exe == main_game_exe(s.game.id)) {
        const GameMapping m = main_game_mapping(s);
        if (m.valid()) return m.deg_per_count();
    }
    if (const auto it = s.app.game_sens.find(exe); it != s.app.game_sens.end()) {
        if (yaw > 0.0 && it->second.sensitivity > 0.0) return yaw * it->second.sensitivity;
        if (it->second.cm360 > 0.0 && s.mouse.dpi > 0) return 360.0 / (it->second.cm360 / 2.54 * double(s.mouse.dpi));
    }
    const GameMapping m = main_game_mapping(s);
    return m.valid() ? m.deg_per_count() : 0.028;
}

}  // namespace

// While the fix is on in a game, FitSuite reads the mouse (movement and clicks only), finds aimed
// flicks, and every few minutes nudges the fix when flicks in some directions keep landing short or
// long compared with the others (fit::LiveTuner: small steps, same bounds as the measured fix).
void App::tick_live() {
    auto& lt = setup_.live;
    MouseProfileRecord* m = active_fix();
    const bool want = lt.enabled && setup_.fix_games.enabled && games_on_ == true && !games_pending_ && m &&
                      !mouse.test && !motion.test && !platform_.dev_harness() && !platform_.window_focused();
    auto& rm = platform_.mouse();
    if (!want) {
        if (live_capturing_) {
            live_capturing_ = false;
            rm.set_capture_in_games(false);
            if (!mouse.test && !motion.test) rm.stop();  // a test that just started keeps its capture
            live_detector_.reset();
        }
        return;
    }
    if (lt.fix_id != m->id) {  // another fix: start counting afresh
        lt = LiveTuningState{};
        lt.fix_id = m->id;
        live_tuner_ = LiveTuner{};
        save();
    }
    if (!live_capturing_) {
        std::string err;
        rm.set_capture_in_games(true);
        if (!rm.active() && !rm.start(&err)) {
            rm.set_capture_in_games(false);
            return;
        }
        live_capturing_ = true;
        FlickDetectorOptions o;
        o.deg_per_count = game_deg_per_count(setup_, games_on_for_);
        live_detector_ = std::make_unique<FlickDetector>(o);
        live_events_.clear();
        rm.drain(live_events_);  // nothing from before the game counts
        live_events_.clear();
        if (live_last_update_ <= 0.0) live_last_update_ = clock_s();
    }
    live_events_.clear();
    rm.drain(live_events_);
    for (const auto& e : live_events_) {
        if (e.injected || e.absolute) continue;
        if (!setup_.mouse.device_id.empty() && e.device_id != setup_.mouse.device_id) continue;  // the measured mouse only
        live_detector_->add({e.t_us, e.dx, e.dy, (e.buttons & kLeftDown) != 0});
    }
    live_detector_->tick(rm.now_us());
    bool counted = false;
    for (const auto& f : live_detector_->take()) {
        live_tuner_.add(f);
        ++lt.flicks;
        switch (f.kind) {
            case LiveFlick::Kind::Good: ++lt.good; break;
            case LiveFlick::Kind::Overshoot: ++lt.overshoot; break;
            case LiveFlick::Kind::Undershoot: ++lt.undershoot; break;
            case LiveFlick::Kind::Sideways: ++lt.sideways; break;
        }
        counted = true;
    }
    if (counted) save();

    // At most every 3 minutes, and only while Raw Accel is not busy.
    if (clock_s() - live_last_update_ < 180.0 || writer_.token) return;
    const auto next = live_tuner_.propose(m->transform.raw_accel);
    if (!next) return;
    live_last_update_ = clock_s();
    auto& r = m->transform.raw_accel;
    if (lt.measured.size() != 5) lt.measured = {r.rotation_deg, r.sens, r.yx_ratio, r.lr_ratio, r.ud_ratio};
    r.rotation_deg = next->rotation_deg;
    r.sens = next->sens;
    r.yx_ratio = next->yx_ratio;
    r.lr_ratio = next->lr_ratio;
    r.ud_ratio = next->ud_ratio;
    ++lt.updates;
    lt.last_update_utc = utc_now_iso();
    save_now();
    std::string err;
    const auto& g = setup_.fix_games;
    if (games_write_corrected(&err))
        rawaccel_run_writer(g.rawaccel_corrected, "game-on", "", &err, std::filesystem::path(g.rawaccel_settings).parent_path());
}

void App::live_reset() {
    auto& lt = setup_.live;
    MouseProfileRecord* m = active_fix();
    if (m && lt.fix_id == m->id && lt.measured.size() == 5) {
        auto& r = m->transform.raw_accel;
        r.rotation_deg = lt.measured[0];
        r.sens = lt.measured[1];
        r.yx_ratio = lt.measured[2];
        r.lr_ratio = lt.measured[3];
        r.ud_ratio = lt.measured[4];
    }
    const bool enabled = lt.enabled;
    lt = LiveTuningState{};
    lt.enabled = enabled;
    if (m) lt.fix_id = m->id;
    live_tuner_ = LiveTuner{};
    live_last_update_ = clock_s();
    save_now();
    toast(ui::Tone::Info, "Your fix is back to the measured one. It is used the next time a game comes to the front.");
}

App::GamesState App::games_state() const {
    GamesState s;
    s.enabled = setup_.fix_games.enabled;
    for (const auto& m : setup_.mouse_profiles)
        if (m.id == setup_.active_mouse_profile && m.transform.kind == TransformKind::RawAccel) s.have_fix = true;
    s.fix_on = games_on_;
    s.switching = games_pending_.has_value();
    s.writer_slow = writer_.token && clock_s() - writer_.started > 10.0;
    s.foreground = games_fg_;
    s.game = games_on_ == true ? games_on_for_ : std::string();
    s.error = games_error_;
    return s;
}

std::string App::tray_text() const {
    if (!setup_.fix_games.enabled) return "FitSuite";
    if (!games_error_.empty()) return "FitSuite: switching your fix is paused";
    if (!games_state().have_fix) return "FitSuite: no fix chosen";
    if (games_on_ == true) return "FitSuite: fix on in " + fit::game_display_name(games_on_for_);
    return "FitSuite: fix off, waiting for your games";
}

double App::seconds_until_next_tick() const {
    double s = 60.0;  // reminders and housekeeping
    if (writer_.token) s = std::min(s, 0.1);
    if (save_pending_) s = std::min(s, std::max(0.0, save_due_ - clock_s()));
    if (games_restore_needed_) s = 0.0;
    if (update_busy() || ra_install.token) s = std::min(s, 0.1);
    if (live_capturing_) s = std::min(s, 0.25);  // live tuning reads the mouse while a game is in front
    if (setup_.fix_games.enabled && games_error_.empty()) {
        // Woken at once when another program comes to the front; this is the fallback poll.
        s = std::min(s, 2.0);
        if (auto due = switcher_.deadline()) s = std::min(s, std::max(0.02, *due - clock_s()));
    }
    return std::max(0.0, s);
}

void App::background_tick() {
    platform_.set_keep_running(keep_running());
    tick_writer();
    tick_games();
    tick_live();
    tick_ra_install();
    tick_updater();
    if (save_pending_ && clock_s() >= save_due_) save_now();
}

void App::on_hidden() {
    revert_pending_display(*this);
    if (mouse.test && mouse.test->running()) mouse.test->abort("FitSuite was closed to the notification area.");
    if (motion.test && motion.test->running()) motion.test->abort("FitSuite was closed to the notification area.");
    if (mouse.test) on_mouse_test_done();
    if (motion.test) on_motion_test_done();
}

// ---- shared helpers -----------------------------------------------------------------------------

std::string short_time(const std::string& iso) {
    if (iso.size() < 16) return iso;
    return iso.substr(0, 10) + " " + iso.substr(11, 5) + " UTC";
}

ui::Origin origin_of(const std::string& origin) {
    return origin == "simulated" ? ui::Origin::Simulated : ui::Origin::Measured;
}

void staleness_line(App& app, const Conditions& recorded, const std::string& tool) {
    Conditions current = conditions_for(tool, app.setup());
    const Staleness s = check_staleness(recorded, current);
    if (!s.outdated) return;
    std::string why;
    for (const auto& r : s.reasons) why += (why.empty() ? "" : "; ") + r;
    ui::pill("POTENTIALLY OUTDATED", theme::kWarn);
    ImGui::SameLine();
    ui::muted("%s", why.c_str());
}

}  // namespace fitapp
