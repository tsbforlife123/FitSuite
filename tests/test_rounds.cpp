// Round-based calibration (format 2): plans, segmentation with a moving start, the fit on round
// data, the aim report and tracking analysis, and storage.
#include <algorithm>
#include <cmath>
#include <vector>

#include "doctest.h"
#include "fit/aim_report.hpp"
#include "fit/mouse_analysis.hpp"
#include "fit/mouse_fit.hpp"
#include "fit/mouse_schedule.hpp"
#include "fit/serialize.hpp"
#include "sim_user.hpp"

using namespace fit;

namespace {

// Plays a round-based plan the way the app does: targets relative to the crosshair, "back" targets
// placed at the round's centre, the next target appearing as soon as the last one ends, switching
// rounds with three targets at random spots, and a tracking round.
Session simulate_rounds(sim::SimUserParams p, uint64_t seed, const RoundPlanOptions& ro = {}) {
    p.single_click = true;
    if (p.settle_deg == 0.0) p.settle_deg = 0.4;
    const Plan plan = make_round_calibration_plan(ro);
    Session s;
    s.id = "cal";
    s.kind = SessionKind::Calibration;
    s.format = 2;
    s.conditions.mapping.sensitivity = 0.4;
    sim::SimUser u(p, seed * 7919 + 3);
    Rng rng(seed ^ 0x5EED);
    const double k = s.conditions.mapping.deg_per_count();
    int64_t t = 0;
    for (size_t b = 0; b < plan.blocks.size(); ++b) {
        const BlockInfo& bi = plan.block_info[b];
        RoundRecord rr;
        rr.index = int(b);
        rr.task = bi.task;
        t += 1'000'000;
        rr.t_start_us = t;
        double yaw = 0.0, pitch = 0.0;  // crosshair, relative to the round's centre
        auto play = [&](TargetSpec spec) {
            spec.block = int(b);
            u.params().timeout_ms = bi.target_timeout_ms;
            Trial tr = u.run_trial(spec, s.conditions.mapping, Transform{}, yaw, pitch, t);
            tr.timeout_ms = bi.target_timeout_ms;
            Vec2 sum{};
            for (const auto& m : tr.motion) sum += Vec2{m.out_dx, m.out_dy};
            yaw += sum.x * k;
            pitch -= sum.y * k;
            t = tr.t_end_us;
            if (tr.outcome == TrialOutcome::Hit) ++rr.hits;
            else if (tr.outcome == TrialOutcome::Miss) ++rr.misses;
            else ++rr.timeouts;
            s.trials.push_back(std::move(tr));
        };
        if (bi.task == Task::Flick || bi.task == Task::Micro) {
            for (TargetSpec spec : plan.blocks[b]) {
                if (spec.to_anchor) {  // back to the centre, from wherever the crosshair is
                    spec.dir_deg = std::atan2(-pitch, -yaw) * kRadToDeg;
                    spec.dist_deg = std::hypot(yaw, pitch);
                    if (spec.dist_deg < 0.5) continue;
                }
                play(spec);
            }
        } else if (bi.task == Task::Switch) {
            for (int kill = 0; kill < bi.kills; ++kill) {
                const double ty = rng.uniform(-18.0, 18.0), tp = rng.uniform(-9.0, 9.0);
                TargetSpec spec;
                spec.task = Task::Switch;
                spec.radius_deg = bi.radius_deg;
                spec.dir_deg = std::atan2(tp - pitch, ty - yaw) * kRadToDeg;
                spec.dist_deg = std::hypot(ty - yaw, tp - pitch);
                if (spec.dist_deg < 1.5) continue;
                play(spec);
            }
        } else {  // tracking: the same target movement as the test (TrackMotion)
            double ay = 0, ap = 0;
            TrackMotion m;
            const double dt = 1.0 / 240.0;
            for (double ts = 0; ts < bi.duration_ms / 1000.0; ts += dt) {
                m.step(ts, dt, rng);
                const double ty = m.yaw, tp = m.pitch, vy = m.vyaw, vp = m.vpitch;
                const Vec2 c = u.track_step(ts, dt, ty, tp, vy, vp, ay, ap, s.conditions.mapping);
                ay += c.x * k;
                ap -= c.y * k;
                TrackSample x;
                x.t_us = t + int64_t(ts * 1e6);
                x.round = int(b);
                x.target_yaw = float(ty);
                x.target_pitch = float(tp);
                x.aim_yaw = float(ay);
                x.aim_pitch = float(ap);
                x.target_vyaw = float(vy);
                x.target_vpitch = float(vp);
                x.radius = float(bi.radius_deg);
                s.tracking.push_back(x);
            }
            t += int64_t(bi.duration_ms * 1000.0);
        }
        rr.t_end_us = t;
        rr.complete = true;
        s.rounds.push_back(rr);
    }
    s.complete = true;
    return s;
}

}  // namespace

TEST_CASE("the round plan has flick, micro, flick, tracking and switching rounds of out-and-back pairs") {
    const Plan p = make_round_calibration_plan({});
    REQUIRE(p.format() == 2);
    REQUIRE(p.blocks.size() == 5);
    CHECK(p.block_info[0].task == Task::Flick);
    CHECK(p.block_info[1].task == Task::Micro);
    CHECK(p.block_info[2].task == Task::Flick);
    CHECK(p.block_info[3].task == Task::Track);
    CHECK(p.block_info[4].task == Task::Switch);
    CHECK(p.blocks[0].size() == 64);  // 8 directions × 4 distances, out and back
    for (size_t i = 0; i + 1 < p.blocks[0].size(); i += 2) {
        CHECK_FALSE(p.blocks[0][i].to_anchor);
        CHECK(p.blocks[0][i + 1].to_anchor);
        CHECK(p.blocks[0][i + 1].nominal_dist_deg == doctest::Approx(p.blocks[0][i].dist_deg));
    }
    CHECK(p.scored_trials() == 64 + 32 + 64 + 36);
    // Validation blocks keep 16 targets each, now out and back.
    const Plan v = make_validation_plan({});
    CHECK(v.format() == 2);
    for (const auto& b : v.blocks) CHECK(b.size() == 16);
}

TEST_CASE("a miss counts like running out of time") {
    sim::SimUserParams p;
    p.single_click = true;
    p.early_click_prob = 1.0;  // clicks as soon as it is close, so some clicks miss
    p.radial_sd = 0.25;
    sim::SimUser u(p, 5);
    GameMapping map;
    int misses = 0;
    for (int i = 0; i < 200; ++i) {
        TargetSpec spec;
        spec.dir_deg = 45.0 * (i % 8);
        spec.dist_deg = 15.0;
        Trial tr = u.run_trial(spec, map, Transform{}, 0, 0, 0);
        tr.timeout_ms = 2200;
        if (tr.outcome != TrialOutcome::Miss) continue;
        ++misses;
        CHECK(tr.clicks.size() == 1);
        const TrialMetrics m = analyze_trial(tr, map);
        CHECK_FALSE(m.hit);
        CHECK(m.acquisition_ms >= 2200.0);
    }
    CHECK(misses > 10);
}

TEST_CASE("leftover movement from the last target does not bias the flick measurement") {
    // With and without settling motion, an unbiased player's flicks should measure the same.
    auto mean_errors = [](double settle) {
        sim::SimUserParams p;
        p.settle_deg = settle;
        sim::SimUser u(p, 11);
        GameMapping map;
        double re = 0, ae = 0;
        int n = 0;
        for (int i = 0; i < 600; ++i) {
            TargetSpec spec;
            spec.dir_deg = 45.0 * (i % 8);
            spec.dist_deg = 8.0 + 6.0 * (i % 3);
            const TrialMetrics m = analyze_trial(u.run_trial(spec, map, Transform{}, 0, 0, 0), map);
            if (!m.valid) continue;
            re += m.radial_error_pct;
            ae += m.angle_error_deg;
            ++n;
        }
        return std::pair<double, double>{re / n, ae / n};
    };
    const auto still = mean_errors(0.0);
    const auto moving = mean_errors(0.6);
    MESSAGE("mean distance error: still " << still.first << "%, settling " << moving.first << "%; angle " << still.second
                                          << "° vs " << moving.second << "°");
    CHECK(std::fabs(still.first - moving.first) < 1.5);
    CHECK(std::fabs(moving.second) < 0.5);
}

TEST_CASE("round-based calibration finds a vertical undershoot and rarely invents one") {
    sim::SimUserParams biased;
    biased.m11 = 0.90;  // up and down flicks land 10% short
    int found = 0;
    for (uint64_t i = 0; i < 12; ++i) {
        const FitResult r = fit_profiles(simulate_rounds(biased, 300 + i));
        if (!r.ok) {
            MESSAGE("fit failed: " << r.message);
            for (const auto& d : r.data_problems) MESSAGE("  " << d);
            MESSAGE("used " << r.n_used << " invalid " << r.n_invalid << " gross " << r.n_excluded_gross << " of " << r.n_trials);
        }
        REQUIRE(r.ok);
        if (r.raw_accel_recommended && r.raw_accel.yx_ratio > 1.03) ++found;
    }
    MESSAGE("10% vertical undershoot found in " << found << " of 12 round-based calibrations");
    CHECK(found >= 11);

    int invented = 0;
    const int sims = 60;
    for (int i = 0; i < sims; ++i) {
        const FitResult r = fit_profiles(simulate_rounds({}, 900 + uint64_t(i)));
        if (!r.ok) {
            MESSAGE("fit failed: " << r.message);
            for (const auto& d : r.data_problems) MESSAGE("  " << d);
        }
        REQUIRE(r.ok);
        if (r.raw_accel_recommended || r.directional_recommended) ++invented;
    }
    MESSAGE("unbiased player: correction proposed in " << invented << " of " << sims << " round-based calibrations");
    CHECK(double(invented) / sims <= 0.08);
}

TEST_CASE("the aim report covers every round and tracking shows a directional lag") {
    sim::SimUserParams p;
    p.down_gain = 0.85;  // falls behind targets moving down
    const Session s = simulate_rounds(p, 77);
    const AimReport rep = make_aim_report(s);
    REQUIRE(rep.rounds.size() == 5);
    for (const auto& r : rep.rounds) {
        if (r.task == Task::Track) {
            REQUIRE(r.tracking.ok);
            CHECK(r.tracking.on_target_pct > 5.0);
            REQUIRE_FALSE(std::isnan(r.tracking.lag_ms[6]));
            REQUIRE_FALSE(std::isnan(r.tracking.lag_ms[2]));
            MESSAGE("lag up " << r.tracking.lag_ms[2] << " ms, down " << r.tracking.lag_ms[6] << " ms");
            CHECK(r.tracking.lag_ms[6] > r.tracking.lag_ms[2] + 30.0);  // trails downward targets more
            MESSAGE("tracking: " << r.tracking.summary);
            CHECK(r.tracking.summary.find("down") != std::string::npos);
        } else {
            CHECK(r.targets > 20);
            CHECK(r.accuracy_pct > 40.0);
            CHECK(r.median_time_ms > 150.0);
        }
    }
    REQUIRE(rep.lines.size() == 5);  // flicks, micro, switching, tracking, flick path
    CHECK(rep.lines.back().rfind("Flick path: ", 0) == 0);
    for (const auto& l : rep.lines) MESSAGE(l);

    const TrackingResult even = analyze_tracking(simulate_rounds({}, 78).tracking);
    REQUIRE(even.ok);
    MESSAGE("unbiased tracking: " << even.summary);
    CHECK(even.summary.find("about equally") != std::string::npos);
}

TEST_CASE("tracking: a crosshair just behind the target is on its trail and counts as on target") {
    // A target moving right at 10°/s; the crosshair follows on the same line, `lag_s` behind it.
    auto run = [](double lag_s) {
        std::vector<TrackSample> v;
        const double speed = 10.0, dt = 1.0 / 240.0;
        for (double ts = 0; ts < 10.0; ts += dt) {
            TrackSample x;
            x.t_us = int64_t(ts * 1e6);
            x.target_yaw = float(speed * ts);
            x.aim_yaw = float(speed * (ts - lag_s));
            x.target_vyaw = float(speed);
            x.radius = 0.6f;
            v.push_back(x);
        }
        return analyze_tracking(v);
    };
    const TrackingResult close = run(0.10);  // 1.0° behind: off the target itself, but on its trail
    REQUIRE(close.ok);
    CHECK(close.on_target_pct > 95.0);
    CHECK(close.mean_error_deg == doctest::Approx(1.0).epsilon(0.05));  // the distance is still measured
    const TrackingResult far = run(0.40);  // 4° behind: past the trail's end too
    REQUIRE(far.ok);
    CHECK(far.on_target_pct < 5.0);
}

TEST_CASE("tracking motion eases into new directions and stays in its box") {
    Rng rng(9);
    TrackMotion m;
    const double dt = 1.0 / 240.0;
    double max_jump = 0.0, max_speed = 0.0;
    double pvy = 0, pvp = 0;
    for (double ts = 0; ts < 60.0; ts += dt) {
        m.step(ts, dt, rng);
        CHECK(std::fabs(m.yaw) <= TrackMotion::kBoxYaw + 0.1);
        CHECK(std::fabs(m.pitch) <= TrackMotion::kBoxPitch + 0.1);
        // Change of velocity per frame, not counting bounces off the box edge (a component changes sign).
        const bool bounced = pvy * m.vyaw < 0 || pvp * m.vpitch < 0;
        if (!bounced) max_jump = std::max(max_jump, std::hypot(m.vyaw - pvy, m.vpitch - pvp));
        max_speed = std::max(max_speed, std::hypot(m.vyaw, m.vpitch));
        pvy = m.vyaw, pvp = m.vpitch;
    }
    MESSAGE("largest speed change in one frame " << max_jump << " °/s, top speed " << max_speed << " °/s");
    CHECK(max_jump < 2.0);  // smooth: at most a few °/s per frame, never a jump to a new speed
    CHECK(max_speed <= TrackMotion::kMaxSpeed + 1e-6);
}

TEST_CASE("round-based sessions round-trip through their file format") {
    const Session s = simulate_rounds({}, 5);
    std::vector<std::string> errs;
    const auto q = session_from_json(to_json(s), errs);
    REQUIRE(q);
    CHECK(errs.empty());
    CHECK(q->format == 2);
    REQUIRE(q->rounds.size() == s.rounds.size());
    CHECK(q->rounds[3].task == Task::Track);
    REQUIRE(q->tracking.size() == s.tracking.size());
    CHECK(q->tracking[100].target_yaw == doctest::Approx(s.tracking[100].target_yaw).epsilon(1e-3));
    CHECK(q->tracking[100].t_us == s.tracking[100].t_us);
    REQUIRE(q->trials.size() == s.trials.size());
    int misses = 0;
    for (size_t i = 0; i < s.trials.size(); ++i) {
        CHECK(q->trials[i].outcome == s.trials[i].outcome);
        CHECK(q->trials[i].spec.task == s.trials[i].spec.task);
        CHECK(q->trials[i].timeout_ms == doctest::Approx(s.trials[i].timeout_ms));
        misses += s.trials[i].outcome == TrialOutcome::Miss;
    }
    CHECK(misses > 0);
}
