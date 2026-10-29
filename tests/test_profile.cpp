#include <filesystem>
#include <fstream>

#include "doctest.h"
#include "fit/serialize.hpp"
#include "fit/store.hpp"
#include "fit/timeutil.hpp"
#include "sim_user.hpp"

using namespace fit;
namespace fs = std::filesystem;

namespace {

fs::path temp_dir(const char* name) {
    fs::path p = fs::temp_directory_path() / ("fitsuite-test-" + std::string(name) + "-" + make_id("t").substr(2, 8));
    fs::create_directories(p);
    return p;
}

SetupProfile sample_setup() {
    SetupProfile p = make_default_setup();
    p.mouse.name = "Test Mouse";
    p.mouse.vid_pid = "046D:C094";
    p.mouse.dpi = 800;
    p.game.sensitivity = 0.35;
    p.pc.cpu = "Test CPU";
    p.pc.gpus.push_back({"Test GPU", "31.0.15.5222", 0x10DE, 0x2684, 24});
    p.display.name = "Test Monitor";
    p.display.width = 1920;
    p.display.height = 1080;
    p.display.refresh_hz = 239.76;
    p.display.modes_hz = {60, 144, 239.76};
    MouseProfileRecord m;
    m.id = "mp-1";
    m.label = "Directional";
    m.algorithm_version = kMouseFitAlgorithmVersion;
    m.transform.kind = TransformKind::Directional;
    m.transform.directional.gain_cos[1] = -0.03;
    m.transform.directional.angle_const = -0.02;
    m.transform.directional.strength = 0.75;
    m.conditions = conditions_for("mousefit", p);
    p.mouse_profiles.push_back(m);
    FrameConfig fc;
    fc.id = "fc-1";
    fc.label = "Baseline";
    fc.baseline = true;
    FrameRun r;
    r.id = "run-1";
    r.metrics.avg_fps = 300;
    r.metrics.low1_fps = 180;
    r.metrics.p99_ms = 6.1;
    r.metrics.stutters_per_min = 0.4;
    fc.runs.push_back(r);
    p.frame_configs.push_back(fc);
    p.applied_changes.push_back({"ch-1", "display.refresh_rate", "\\\\.\\DISPLAY1", "144", "240", utc_now_iso(), "", "applied", true});
    return p;
}

}  // namespace

TEST_CASE("setup profile round-trips through JSON") {
    const SetupProfile p = sample_setup();
    std::vector<std::string> errs;
    auto q = setup_from_json(to_json(p), errs);
    REQUIRE_MESSAGE(q, (errs.empty() ? "" : errs.front()));
    CHECK(q->mouse.vid_pid == "046D:C094");
    CHECK(q->display.modes_hz.size() == 3);
    REQUIRE(q->mouse_profiles.size() == 1);
    CHECK(q->mouse_profiles[0].transform.directional.strength == doctest::Approx(0.75));
    CHECK(q->frame_configs[0].runs[0].metrics.p99_ms == doctest::Approx(6.1));
    CHECK(std::isnan(q->frame_configs[0].runs[0].metrics.latency_p50_ms));  // missing stays missing
    CHECK(q->applied_changes[0].after == "240");
}

TEST_CASE("the per-game fix settings round-trip, and old files load without them") {
    SetupProfile p = sample_setup();
    p.fix_games.enabled = true;
    p.fix_games.games = {"valorant-win64-shipping.exe", "cs2.exe"};
    p.fix_games.rawaccel_settings = "C:/RawAccel/settings.json";
    p.fix_games.rawaccel_corrected = "C:/RawAccel/settings.fitsuite-20261026-010000.json";
    p.fix_games.start_with_windows = true;
    std::vector<std::string> errs;
    auto q = setup_from_json(to_json(p), errs);
    REQUIRE(q);
    CHECK(q->fix_games.enabled);
    CHECK(q->fix_games.games.size() == 2);
    CHECK(q->fix_games.rawaccel_corrected == p.fix_games.rawaccel_corrected);
    CHECK(q->fix_games.start_with_windows);

    json old = to_json(sample_setup());
    old.erase("fix_games");
    auto r = setup_from_json(old, errs);
    REQUIRE(r);
    CHECK_FALSE(r->fix_games.enabled);
    REQUIRE(r->fix_games.games.size() == 1);  // the default: VALORANT
    CHECK(r->fix_games.games.front() == "valorant-win64-shipping.exe");

    SetupProfile none = sample_setup();
    none.fix_games.games.clear();  // the player unticked every game
    auto n = setup_from_json(to_json(none), errs);
    REQUIRE(n);
    CHECK(n->fix_games.games.empty());

    json broken = to_json(p);
    broken["fix_games"]["rawaccel_corrected"] = "";  // enabled but nothing to switch to
    auto b = setup_from_json(broken, errs);
    REQUIRE(b);
    CHECK_FALSE(b->fix_games.enabled);
}

TEST_CASE("settings, mouse model and the last quick check round-trip; old files get sensible settings") {
    SetupProfile p = sample_setup();
    p.mouse.model = "Logitech G Pro X Superlight 2";
    p.app.theme = "dark";
    p.app.start_with_windows = true;
    p.app.start_hidden = false;
    p.app.close_to_tray = false;
    p.app.show_extra_tools = true;
    p.app.check_reminder_days = 30;
    p.app.game_sens["cs2.exe"] = {1.27, 0.0};
    p.app.game_sens["fortniteclient-win64-shipping.exe"] = {0.0, 38.5};
    p.app.refresh_kept = {"DEL4321@144"};
    p.app.setup_done = false;
    p.app.setup_step = 3;
    p.app.rawaccel_install_pending = true;
    p.app.update_check = false;
    p.app.notes_hidden = "0.2.1";
    p.app.auto_install_updates = true;
    p.app.pending_update_exe = "C:/x/update/FitSuite-v1.0.1.exe";
    p.app.pending_update_sha = std::string(64, 'a');
    p.app.pending_update_version = "1.0.1";
    MouseProfileRecord m;
    m.id = "mp1";
    m.label = "fix";
    m.algorithm_version = "mousefit-1.0";
    CheckSummary c;
    c.session_id = "chk1";
    c.verdict = "still_fits";
    c.max_residual_pct = 2.5;
    m.last_check = c;
    p.mouse_profiles.push_back(m);
    std::vector<std::string> errs;
    auto q = setup_from_json(to_json(p), errs);
    REQUIRE(q);
    CHECK(q->mouse.model == p.mouse.model);
    CHECK(q->app.theme == "dark");
    CHECK(q->app.start_with_windows);
    CHECK_FALSE(q->app.start_hidden);
    CHECK_FALSE(q->app.close_to_tray);
    CHECK(q->app.show_extra_tools);
    CHECK(q->app.check_reminder_days == 30);
    CHECK(q->app.game_sens.at("cs2.exe").sensitivity == doctest::Approx(1.27));
    CHECK(q->app.game_sens.at("fortniteclient-win64-shipping.exe").cm360 == doctest::Approx(38.5));
    REQUIRE(q->app.refresh_kept.size() == 1);
    CHECK(q->app.refresh_kept.front() == "DEL4321@144");
    CHECK_FALSE(q->app.setup_done);
    CHECK(q->app.setup_step == 3);
    CHECK(q->app.rawaccel_install_pending);
    CHECK_FALSE(q->app.update_check);
    CHECK(q->app.notes_hidden == "0.2.1");
    CHECK(q->app.auto_install_updates);
    CHECK(q->app.pending_update_exe == "C:/x/update/FitSuite-v1.0.1.exe");
    CHECK(q->app.pending_update_sha == std::string(64, 'a'));
    CHECK(q->app.pending_update_version == "1.0.1");
    REQUIRE(q->mouse_profiles.back().last_check);
    CHECK(q->mouse_profiles.back().last_check->verdict == "still_fits");

    // A file from before the Settings tab: defaults, and "Start with Windows" carried over from the
    // per-game fix.
    json old = to_json(sample_setup());
    old.erase("app");
    old["fix_games"]["enabled"] = true;
    old["fix_games"]["rawaccel_settings"] = "C:/RawAccel/settings.json";
    old["fix_games"]["rawaccel_corrected"] = "C:/x/rawaccel-with-fix.json";
    old["fix_games"]["start_with_windows"] = true;
    auto o = setup_from_json(old, errs);
    REQUIRE(o);
    CHECK(o->app.theme == "system");
    CHECK(o->app.start_with_windows);
    CHECK(o->app.close_to_tray);
    CHECK_FALSE(o->app.show_extra_tools);
    CHECK(o->app.check_reminder_days == 7);
    CHECK(o->app.setup_done == !o->mouse_sessions.empty());  // only players who already measured skip the setup
    CHECK(o->app.update_check);
    CHECK(o->app.notes_hidden.empty());
    CHECK_FALSE(o->app.auto_install_updates);  // asks Update / Not now unless the player chose otherwise  // the notes show until the player clicks "Don't show again"

    json bad = to_json(p);
    bad["app"]["theme"] = "neon";  // unknown theme falls back to following Windows
    auto b = setup_from_json(bad, errs);
    REQUIRE(b);
    CHECK(b->app.theme == "system");
}

TEST_CASE("validation rejects malformed or unsafe profiles with readable errors") {
    std::vector<std::string> errs;
    SUBCASE("wrong schema") {
        json j = to_json(sample_setup());
        j["schema"] = "something.else";
        CHECK_FALSE(setup_from_json(j, errs));
        CHECK(errs.front().find("not a fitsuite.setup") != std::string::npos);
    }
    SUBCASE("newer format") {
        json j = to_json(sample_setup());
        j["schema_version"] = 99;
        CHECK_FALSE(setup_from_json(j, errs));
        CHECK(errs.front().find("newer version") != std::string::npos);
    }
    SUBCASE("out of range DPI") {
        json j = to_json(sample_setup());
        j["hardware"]["mouse"]["dpi"] = 5;
        CHECK_FALSE(setup_from_json(j, errs));
        CHECK(errs.front().find("hardware.mouse.dpi") != std::string::npos);
    }
    SUBCASE("wrong type") {
        json j = to_json(sample_setup());
        j["game"]["sensitivity"] = "fast";
        CHECK_FALSE(setup_from_json(j, errs));
        CHECK(errs.front().find("game.sensitivity: expected a number") != std::string::npos);
    }
    SUBCASE("correction beyond the safety limit") {
        json j = to_json(sample_setup());
        j["mousefit"]["profiles"][0]["transform"]["directional"]["gain_cos"] = {0.0, 0.4};
        CHECK_FALSE(setup_from_json(j, errs));
        CHECK(errs.front().find("safety limit") != std::string::npos);
    }
    SUBCASE("dangling active profile") {
        json j = to_json(sample_setup());
        j["mousefit"]["active_profile"] = "does-not-exist";
        CHECK_FALSE(setup_from_json(j, errs));
    }
}

TEST_CASE("mouse profile export/import; imports are never active") {
    SetupProfile p = sample_setup();
    p.mouse_profiles[0].status = "active";
    std::vector<std::string> errs;
    auto m = import_mouse_profile(export_mouse_profile(p.mouse_profiles[0]), errs);
    REQUIRE(m);
    CHECK(m->status == "candidate");
    json bad = export_mouse_profile(p.mouse_profiles[0]);
    bad["profile"]["transform"]["kind"] = "teleport";
    CHECK_FALSE(import_mouse_profile(bad, errs));
    json ra = export_mouse_profile(p.mouse_profiles[0]);
    ra["profile"]["transform"] = {{"kind", "rawaccel"},
                                  {"raw_accel", {{"rotation_deg", 20}, {"sens", 1}, {"yx_ratio", 1}, {"lr_ratio", 1}, {"ud_ratio", 1}}}};
    errs.clear();
    CHECK_FALSE(import_mouse_profile(ra, errs));
}

TEST_CASE("staleness: changed hardware or settings flag results as outdated") {
    SetupProfile p = sample_setup();
    const Conditions recorded = conditions_for("mousefit", p);
    CHECK_FALSE(check_staleness(recorded, conditions_for("mousefit", p)).outdated);
    p.mouse.dpi = 1600;
    auto s = check_staleness(recorded, conditions_for("mousefit", p));
    CHECK(s.outdated);
    REQUIRE(s.reasons.size() == 1);
    CHECK(s.reasons[0] == "DPI changed from 800 to 1600");
    // FrameFit results do not care about DPI, but do care about the GPU driver.
    const Conditions frame = conditions_for("framefit", p);
    p.pc.gpus[0].driver = "32.0.15.6070";
    CHECK(check_staleness(frame, conditions_for("framefit", p)).outdated);
}

TEST_CASE("atomic saves keep backups; a corrupt file is recovered and kept") {
    const fs::path dir = temp_dir("store");
    Store st(dir);
    SetupProfile p = sample_setup();
    std::string err;
    REQUIRE(st.save_setup(p, &err));
    p.name = "Second";
    REQUIRE(st.save_setup(p, &err));
    CHECK(st.list_backups().size() == 1);
    // No temporary files are left behind.
    for (const auto& e : fs::directory_iterator(dir)) CHECK(e.path().filename().string().find(".tmp-") == std::string::npos);

    auto ok = st.load_setup();
    REQUIRE(ok.setup);
    CHECK(ok.setup->name == "Second");

    {
        std::ofstream f(st.setup_path(), std::ios::trunc);
        f << "{ \"schema\": \"fitsuite.setup\", truncated";
    }
    auto rec = st.load_setup();
    REQUIRE(rec.setup);
    CHECK(rec.recovered_from_backup);
    CHECK_FALSE(rec.errors.empty());
    CHECK_FALSE(rec.corrupt_kept_as.empty());
    CHECK(fs::exists(dir / rec.corrupt_kept_as));
    CHECK(rec.setup->name == "My setup");  // the first save, from the backup
    fs::remove_all(dir);
}

TEST_CASE("first run creates a default setup") {
    const fs::path dir = temp_dir("fresh");
    Store st(dir);
    auto r = st.load_setup();
    REQUIRE(r.setup);
    CHECK(r.created_new);
    fs::remove_all(dir);
}

TEST_CASE("recorded sessions round-trip with every raw report") {
    const fs::path dir = temp_dir("session");
    Store st(dir);
    Session s;
    s.id = "sess-abc";
    s.kind = SessionKind::Calibration;
    s.algorithm_version = kMouseFitAlgorithmVersion;
    s.seed = 1234567890123ull;
    s.conditions.mapping.sensitivity = 0.35;
    s.conditions.data_origin = "simulated";
    sim::SimUser u({}, 4);
    TargetSpec spec;
    spec.dist_deg = 14;
    spec.dir_deg = 135;
    s.trials.push_back(u.run_trial(spec, s.conditions.mapping, {}, 0, 0, 5'000'000));
    std::string err;
    REQUIRE(st.save_session(s, &err));
    std::vector<std::string> errs;
    auto back = st.load_session("sess-abc", errs);
    REQUIRE(back);
    CHECK(back->seed == s.seed);
    REQUIRE(back->trials.size() == 1);
    CHECK(back->trials[0].motion.size() == s.trials[0].motion.size());
    CHECK(back->trials[0].motion.back().t_us == s.trials[0].motion.back().t_us);
    CHECK(back->trials[0].clicks.size() == s.trials[0].clicks.size());
    CHECK(back->conditions.data_origin == "simulated");

    json j = to_json(s);
    j["trials"][0]["motion"][0] = 10'000'000;  // timestamps going backwards
    errs.clear();
    CHECK_FALSE(session_from_json(j, errs));
    fs::remove_all(dir);
}

TEST_CASE("UTC timestamps") {
    CHECK(utc_iso_from_unix(0) == "1970-01-01T00:00:00Z");
    CHECK(utc_iso_from_unix(1792959845) == "2026-10-25T20:24:05Z");
    int64_t t = 0;
    REQUIRE(parse_utc_iso("2026-10-25T20:24:05Z", t));
    CHECK(t == 1792959845);
    CHECK_FALSE(parse_utc_iso("yesterday", t));
}

TEST_CASE("a fix with a speed curve round-trips, and an out-of-limit curve is refused") {
    Transform t;
    t.kind = TransformKind::RawAccel;
    t.raw_accel.yx_ratio = 1.04;
    t.raw_accel.speed.points = {{0.2, 0.95}, {2.0, 1.0}, {9.0, 1.05}, {18.0, 1.05}};
    std::vector<std::string> errs;
    auto back = transform_from_json(to_json(t), "transform", errs);
    REQUIRE_MESSAGE(back, (errs.empty() ? std::string() : errs.front()));
    REQUIRE(back->raw_accel.speed.points.size() == 4);
    CHECK(back->raw_accel.speed.points[2].first == doctest::Approx(9.0));
    CHECK(back->raw_accel.speed.points[2].second == doctest::Approx(1.05));

    // Profiles from before speed curves load without one.
    json old = to_json(t);
    old["raw_accel"].erase("speed_curve");
    back = transform_from_json(old, "transform", errs);
    REQUIRE(back);
    CHECK(back->raw_accel.speed.empty());

    Transform bad = t;
    bad.raw_accel.speed.points[2].second = 1.20;  // beyond the 6% limit
    std::string why;
    CHECK_FALSE(transform_within_limits(bad, &why));
    CHECK(why.find("6%") != std::string::npos);
    bad = t;
    bad.raw_accel.speed.points[2].first = 1.0;  // speeds not increasing
    CHECK_FALSE(transform_within_limits(bad, &why));
}
