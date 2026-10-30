#include "fit/mouse_schedule.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fit {

size_t Plan::scored_trials() const {
    size_t n = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i < block_info.size() && block_info[i].task == Task::Switch) n += size_t(block_info[i].kills);
        else if (i < block_info.size() && block_info[i].task == Task::Track) continue;
        else n += blocks[i].size();
    }
    return n;
}

void TrackMotion::step(double t_s, double dt, Rng& rng) {
    if (t_s >= next_turn_s) {
        const double a = rng.uniform(0.0, kTwoPi), sp = rng.uniform(kMinSpeed, kMaxSpeed);
        goal_vyaw = sp * std::cos(a);
        goal_vpitch = sp * std::sin(a);
        next_turn_s = t_s + rng.uniform(kMinTurnS, kMaxTurnS);
    }
    const double k = 1.0 - std::exp(-std::max(dt, 0.0) / kEaseS);
    vyaw += (goal_vyaw - vyaw) * k;
    vpitch += (goal_vpitch - vpitch) * k;
    // Bounce off the edges of the box (the goal turns round too, so the easing does not push back out).
    if (std::fabs(yaw + vyaw * dt) > kBoxYaw) vyaw = -vyaw, goal_vyaw = -goal_vyaw;
    if (std::fabs(pitch + vpitch * dt) > kBoxPitch) vpitch = -vpitch, goal_vpitch = -goal_vpitch;
    yaw += vyaw * dt;
    pitch += vpitch * dt;
}

std::vector<TargetSpec> out_and_back(const std::vector<TargetSpec>& outward) {
    std::vector<TargetSpec> out;
    out.reserve(outward.size() * 2);
    for (const auto& o : outward) {
        TargetSpec a = o;
        if (a.nominal_dist_deg <= 0.0) a.nominal_dist_deg = a.dist_deg;
        TargetSpec b = a;
        b.to_anchor = true;  // actual dir/dist known only when it appears
        b.dir_deg = std::fmod(a.dir_deg + 180.0, 360.0);
        b.index = a.index + 500;
        out.push_back(a);
        out.push_back(b);
    }
    return out;
}

std::vector<TargetSpec> random_chain(std::vector<TargetSpec> specs, Rng& rng, double max_yaw, double max_pitch) {
    // Random order, picking each next target among those that keep the aim inside the box. A random
    // order can still corner itself near the end, so it starts over (up to 500 times) until the whole
    // chain fits; the best try is used otherwise.
    std::vector<TargetSpec> best_chain;
    double best_total = std::numeric_limits<double>::infinity();
    for (int attempt = 0; attempt < 500; ++attempt) {
        std::vector<TargetSpec> left = specs, out;
        out.reserve(specs.size());
        double yaw = 0.0, pitch = 0.0, total_over = 0.0;
        auto landing = [&](const TargetSpec& s) {
            const double th = s.dir_deg * kDegToRad;
            return std::make_pair(yaw + s.dist_deg * std::cos(th), pitch + s.dist_deg * std::sin(th));
        };
        while (!left.empty()) {
            std::vector<size_t> ok;
            size_t best = 0;
            double best_over = std::numeric_limits<double>::infinity();
            for (size_t i = 0; i < left.size(); ++i) {
                const auto [y, p] = landing(left[i]);
                const double over = std::max(0.0, std::fabs(y) - max_yaw) + std::max(0.0, std::fabs(p) - max_pitch);
                if (over <= 0.0) ok.push_back(i);
                if (over < best_over) best_over = over, best = i;
            }
            const size_t pick = ok.empty() ? best : ok[size_t(rng.below(ok.size()))];
            if (ok.empty()) total_over += best_over;
            const auto [y, p] = landing(left[pick]);
            yaw = y;
            pitch = p;
            out.push_back(left[pick]);
            left.erase(left.begin() + long(pick));
        }
        if (total_over <= 0.0) return out;
        if (total_over < best_total) best_total = total_over, best_chain = out;
    }
    return best_chain;
}

Plan make_round_calibration_plan(const RoundPlanOptions& o) {
    Plan p;
    p.kind = SessionKind::Calibration;
    p.seed = o.seed;
    Rng rng(o.seed ^ 0xA11ull);
    const int dirs = std::max(o.directions, 1);
    int block = 0;
    auto add_pairs = [&](Task task, const std::vector<double>& dists, double radius, double timeout, bool chained = false) {
        std::vector<TargetSpec> outward;
        int i = 0;
        for (int rep = 0; rep < (chained ? 2 : 1); ++rep)  // a chain: every direction and distance twice
        for (int d = 0; d < dirs; ++d)
            for (double dist : dists) {
                TargetSpec s;
                s.dir_deg = 360.0 * d / dirs;
                s.dist_deg = dist;
                s.nominal_dist_deg = dist;
                s.radius_deg = radius;
                s.block = block;
                s.index = block * 1000 + i++;
                s.task = task;
                outward.push_back(s);
            }
        rng.shuffle(outward);
        p.blocks.push_back(chained ? random_chain(outward, rng, o.chain_max_yaw_deg, o.chain_max_pitch_deg)
                                   : out_and_back(outward));
        p.block_condition.push_back(Condition::Baseline);
        BlockInfo bi;
        bi.task = task;
        bi.target_timeout_ms = timeout;
        bi.radius_deg = radius;
        p.block_info.push_back(bi);
        ++block;
    };
    auto add_special = [&](Task task) {
        BlockInfo bi;
        bi.task = task;
        if (task == Task::Switch) {
            bi.target_timeout_ms = o.switch_timeout_ms;
            bi.visible_targets = std::max(1, o.switch_visible);
            bi.kills = std::max(1, o.switch_kills);
            bi.radius_deg = o.switch_radius_deg;
        } else {
            bi.duration_ms = o.track_duration_ms;
            bi.radius_deg = o.track_radius_deg;
        }
        p.blocks.push_back({});
        p.block_condition.push_back(Condition::Baseline);
        p.block_info.push_back(bi);
        ++block;
    };
    // Flick, micro, flick, tracking, switching: the flick rounds bracket the others so fatigue and
    // warm-up spread evenly over the directions.
    add_pairs(Task::Flick, o.flick_distances_deg, o.flick_radius_deg, o.flick_timeout_ms, o.first_flick_round_chained);
    if (o.micro_round) add_pairs(Task::Micro, o.micro_distances_deg, o.micro_radius_deg, o.micro_timeout_ms);
    for (int r = 1; r < o.flick_rounds; ++r) add_pairs(Task::Flick, o.flick_distances_deg, o.flick_radius_deg, o.flick_timeout_ms);
    if (o.track_round) add_special(Task::Track);
    if (o.switch_round) add_special(Task::Switch);
    p.order_label = std::string(p.blocks.size(), 'A');
    return p;
}

Plan make_calibration_plan(const CalibrationPlanOptions& o) {
    Plan p;
    p.kind = SessionKind::Calibration;
    p.seed = o.seed;
    Rng rng(o.seed ^ 0xC0FFEEull);
    const int dirs = std::max(o.directions, 1);
    for (int b = 0; b < std::max(o.blocks, 1); ++b) {
        std::vector<TargetSpec> block;
        int i = 0;
        for (int d = 0; d < dirs; ++d)
            for (double dist : o.distances_deg) {
                TargetSpec s;
                s.dir_deg = 360.0 * d / dirs;
                s.dist_deg = dist;
                s.radius_deg = o.radius_deg;
                s.block = b;
                s.index = b * 1000 + i++;
                s.condition = Condition::Baseline;
                s.purpose = TrialPurpose::Scored;
                block.push_back(s);
            }
        rng.shuffle(block);
        p.blocks.push_back(std::move(block));
        p.block_condition.push_back(Condition::Baseline);
    }
    p.order_label = std::string(p.blocks.size(), 'A');
    return p;
}

Plan make_validation_plan(const ValidationPlanOptions& o) {
    Plan p;
    p.kind = SessionKind::Validation;
    p.seed = o.seed;
    Rng rng(o.seed ^ 0xBADC0DEull);
    const int nblocks = std::max(4, (o.blocks / 4) * 4);
    // ABBA BAAB balances linear and quadratic practice/fatigue trends; which condition comes first
    // is random so the player cannot infer it.
    const bool a_first = rng.below(2) == 0;
    const std::string quad1 = a_first ? "ABBA" : "BAAB";
    const std::string quad2 = a_first ? "BAAB" : "ABBA";
    for (int q = 0; q < nblocks / 4; ++q) p.order_label += (q % 2 == 0) ? quad1 : quad2;

    const int dirs = std::max(o.directions, 1);
    auto make_spec = [&](int block, int i, Condition c, TrialPurpose purpose, int d, double dist) {
        TargetSpec s;
        s.dir_deg = o.direction_offset_deg + 360.0 * d / dirs + rng.uniform(-o.direction_jitter_deg, o.direction_jitter_deg);
        s.dir_deg = std::fmod(s.dir_deg + 360.0, 360.0);
        s.dist_deg = dist * (1.0 + rng.uniform(-o.distance_jitter_frac, o.distance_jitter_frac));
        s.radius_deg = o.radius_deg;
        s.block = block;
        s.index = (block + 1) * 1000 + i;
        s.condition = c;
        s.purpose = purpose;
        return s;
    };
    // Out-and-back pairs: half as many outward targets, each followed by one back to the centre, so a
    // block has as many targets as before and every direction also appears reversed.
    std::vector<TargetSpec> warm;
    for (int i = 0; i < std::max(1, o.warmup_trials / 2); ++i) {
        const double dist = o.distances_deg[size_t(i) % o.distances_deg.size()];
        warm.push_back(make_spec(-1, i, Condition::Baseline, TrialPurpose::Warmup, (i * 3) % dirs, dist));
    }
    rng.shuffle(warm);
    p.warmup = out_and_back(warm);
    p.warmup_info.task = Task::Flick;
    p.warmup_info.target_timeout_ms = o.target_timeout_ms;
    for (int b = 0; b < nblocks; ++b) {
        const Condition c = p.order_label[size_t(b)] == 'A' ? Condition::Baseline : Condition::Candidate;
        std::vector<TargetSpec> outward;
        int i = 0;
        // Alternate the distance levels over the directions, swapping them every other block.
        for (int d = 0; d < dirs; ++d) {
            const size_t li = (size_t(d) + size_t(b)) % o.distances_deg.size();
            outward.push_back(make_spec(b, i++, c, TrialPurpose::Scored, d, o.distances_deg[li]));
        }
        rng.shuffle(outward);
        p.blocks.push_back(out_and_back(outward));
        p.block_condition.push_back(c);
        BlockInfo bi;
        bi.task = Task::Flick;
        bi.target_timeout_ms = o.target_timeout_ms;
        bi.radius_deg = o.radius_deg;
        p.block_info.push_back(bi);
    }
    return p;
}

// ---- TrialPicker ----------------------------------------------------------------------------

TrialPicker::TrialPicker(std::vector<TargetSpec> specs, uint64_t seed, double soft, double hard)
    : remaining_(std::move(specs)), total_(remaining_.size()), rng_(seed ^ 0x51CEull), soft_(soft), hard_(hard) {}

void TrialPicker::complete(int spec_index) {
    auto it = std::find_if(remaining_.begin(), remaining_.end(), [&](const TargetSpec& s) { return s.index == spec_index; });
    if (it != remaining_.end()) {
        remaining_.erase(it);
        consecutive_positioning_ = 0;
    }
}

TargetSpec TrialPicker::next(double pitch) {
    if (remaining_.empty()) return {};
    const TargetSpec* best = nullptr;
    double best_score = std::numeric_limits<double>::infinity();
    for (const auto& s : remaining_) {
        const double th = s.dir_deg * kDegToRad;
        const double after = pitch + s.dist_deg * std::sin(th);
        if (std::fabs(after) > hard_) continue;
        // While pitched, only accept targets that bring the camera back toward (or across) the horizon.
        if (std::fabs(pitch) > soft_ && std::fabs(after) >= std::fabs(pitch) && after * pitch > 0.0) continue;
        // Prefer staying level; avoid long horizontal flicks while pitched; add randomness so the
        // order is not predictable.
        const double score = 0.5 * std::fabs(after) + std::fabs(pitch) * std::fabs(std::cos(th)) + rng_.uniform(0.0, 6.0);
        if (score < best_score) { best_score = score; best = &s; }
    }
    if (best) return *best;

    // Safety valve: never loop on positioning targets.
    if (consecutive_positioning_ >= 2) {
        for (const auto& s : remaining_) {
            const double after = pitch + s.dist_deg * std::sin(s.dir_deg * kDegToRad);
            if (std::fabs(after) < best_score) { best_score = std::fabs(after); best = &s; }
        }
        consecutive_positioning_ = 0;
        return *best;
    }
    ++consecutive_positioning_;

    // Nothing fits from here: position the camera so the most vertical remaining target becomes
    // reachable symmetrically about the horizon. Positioning targets are never scored.
    double v_max = 0.0;
    for (const auto& s : remaining_) {
        const double v = s.dist_deg * std::sin(s.dir_deg * kDegToRad);
        if (std::fabs(v) > std::fabs(v_max)) v_max = v;
    }
    const double goal_pitch = std::clamp(-v_max / 2.0, -hard_, hard_);
    double dp = goal_pitch - pitch;
    double dy = rng_.uniform(2.0, 6.0) * (rng_.below(2) ? 1.0 : -1.0);
    if (std::fabs(dp) < 3.0) dp = dp < 0 ? -3.0 : 3.0;
    TargetSpec pos;
    pos.dir_deg = std::atan2(dp, dy) * kRadToDeg;
    pos.dist_deg = std::hypot(dp, dy);
    pos.radius_deg = remaining_.front().radius_deg;
    pos.block = remaining_.front().block;
    pos.condition = remaining_.front().condition;
    pos.purpose = TrialPurpose::Recenter;
    pos.index = -1 - positioning_counter_++;
    return pos;
}

}  // namespace fit
