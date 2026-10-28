#include "fit/mouse_compare.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "fit/rng.hpp"

namespace fit {

const char* to_string(Verdict v) {
    switch (v) {
        case Verdict::InsufficientData: return "insufficient-data";
        case Verdict::Improved: return "improved";
        case Verdict::NoReliableDifference: return "no-reliable-difference";
        case Verdict::Worse: return "worse";
    }
    return "insufficient-data";
}

namespace {

// One scored, valid trial as used by the comparison.
struct Obs {
    const TrialMetrics* m;
    double y;    // log aim time, adjusted for target distance and direction
    bool has_y;  // false for a miss (the target disappeared): it counts in the hit rate only
    double hit;  // first click hit (0/1)
    double err;  // first-flick endpoint error, degrees
};

struct Block {
    int index = 0;
    Condition condition = Condition::Baseline;
    std::vector<size_t> obs;  // indices into the observation list
};

struct Agg {
    double y = 0.0, hit = 0.0, err = 0.0;
    size_t n = 0, ny = 0;
    void add(const Obs& o) {
        if (o.has_y) {
            y += o.y;
            ++ny;
        }
        hit += o.hit;
        err += o.err;
        ++n;
    }
    double mean_y() const { return ny ? y / double(ny) : 0.0; }
    double hit_rate() const { return n ? hit / double(n) : 0.0; }
    double mean_err() const { return n ? err / double(n) : 0.0; }
};

ConditionSummary summarize(const std::vector<const Block*>& blocks, const std::vector<Obs>& obs) {
    ConditionSummary s;
    std::vector<double> acq, cor, rad, ang, logacq, hits, errs;
    for (const auto* b : blocks) {
        ++s.n_blocks;
        for (size_t i : b->obs) {
            const TrialMetrics& m = *obs[i].m;
            ++s.n_trials;
            ++s.n_valid;
            if (!m.hit) ++s.n_timeouts;
            acq.push_back(m.acquisition_ms);
            logacq.push_back(std::log(std::max(m.acquisition_ms, 1.0)));
            if (m.hit) cor.push_back(m.correction_ms);
            rad.push_back(std::fabs(m.radial_error_pct));
            ang.push_back(std::fabs(m.angle_error_deg));
            hits.push_back(obs[i].hit);
            errs.push_back(obs[i].err);
        }
    }
    if (!acq.empty()) {
        s.geo_mean_acquisition_ms = std::exp(mean(logacq));
        s.median_acquisition_ms = median(acq);
        s.first_click_hit_rate = mean(hits);
        s.mean_endpoint_error_deg = mean(errs);
        s.mean_abs_radial_error_pct = mean(rad);
        s.mean_abs_angle_error_deg = mean(ang);
        s.median_correction_ms = cor.empty() ? 0.0 : median(cor);
    }
    return s;
}

Agg resample(const std::vector<const Block*>& blocks, const std::vector<Obs>& obs, Rng& rng) {
    Agg a;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const Block* b = blocks[size_t(rng.below(blocks.size()))];
        const size_t n = b->obs.size();
        for (size_t j = 0; j < n; ++j) a.add(obs[b->obs[size_t(rng.below(n))]]);
    }
    return a;
}

std::string pct(double ratio) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f%%", std::fabs(ratio - 1.0) * 100.0);
    return buf;
}

// Removes the part of log aim time explained by the target layout (distance, direction). The
// layout is balanced between conditions by design, so this only removes noise, not the effect.
void adjust_for_layout(std::vector<Obs>& obs) {
    const size_t n = obs.size(), p = 6;
    if (n <= p + 2) return;
    Matrix A(p, p);
    std::vector<double> b(p, 0.0);
    auto row = [](const TrialMetrics& m, double* x) {
        const double th = m.target_angle_rad;
        x[0] = 1.0;
        x[1] = std::log(std::max(m.spec.dist_deg, 0.1));
        x[2] = std::cos(th);
        x[3] = std::sin(th);
        x[4] = std::cos(2 * th);
        x[5] = std::sin(2 * th);
    };
    double x[6];
    for (const auto& o : obs) {
        if (!o.has_y) continue;
        row(*o.m, x);
        for (size_t r = 0; r < p; ++r) {
            b[r] += x[r] * o.y;
            for (size_t c = 0; c < p; ++c) A(r, c) += x[r] * x[c];
        }
    }
    auto beta = solve(A, b);
    if (!beta) return;
    for (auto& o : obs) {
        row(*o.m, x);
        double f = 0.0;
        for (size_t c = 1; c < p; ++c) f += x[c] * (*beta)[c];  // keep the intercept: values stay in log-ms
        o.y -= f;
    }
}

}  // namespace

ComparisonResult compare_session(const Session& s, const CompareOptions& opt) {
    ComparisonResult r;
    if (s.kind != SessionKind::Validation) {
        r.message = "Only validation sessions can be compared.";
        return r;
    }
    std::vector<TrialMetrics> metrics;
    metrics.reserve(s.trials.size());
    for (const auto& t : s.trials)
        if (t.spec.purpose == TrialPurpose::Scored) metrics.push_back(analyze_trial(t, s.conditions.mapping, opt.segment));

    std::vector<Obs> obs;
    std::map<int, Block> blocks;
    int excluded = 0;
    for (const auto& m : metrics) {
        Block& b = blocks[m.spec.block];
        b.index = m.spec.block;
        b.condition = m.spec.condition;
        if (!m.valid) { ++excluded; continue; }
        // Aim time: movement onset to the hit (timeouts count as their full duration), so reaction
        // time — which a mouse profile cannot change — does not add noise. A miss that made the
        // target disappear has no aim time; it counts against the hit rate instead.
        const double aim_ms = std::max(m.acquisition_ms - m.reaction_ms, 50.0);
        b.obs.push_back(obs.size());
        obs.push_back({&m, std::log(aim_ms), !m.ended_by_miss, m.first_click_hit ? 1.0 : 0.0, m.endpoint_error_deg});
    }
    adjust_for_layout(obs);

    std::vector<const Block*> A, B;
    for (const auto& kv : blocks) {
        if (kv.second.obs.empty()) continue;
        (kv.second.condition == Condition::Baseline ? A : B).push_back(&kv.second);
    }
    r.baseline = summarize(A, obs);
    r.candidate = summarize(B, obs);
    r.ok = true;

    if (r.baseline.n_valid < opt.min_valid_per_condition || r.candidate.n_valid < opt.min_valid_per_condition ||
        A.size() < 2 || B.size() < 2) {
        r.verdict = Verdict::InsufficientData;
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "Not enough valid trials to compare (%d baseline, %d candidate; at least %d each across two or "
                      "more blocks are needed). Keep your baseline for now.",
                      r.baseline.n_valid, r.candidate.n_valid, opt.min_valid_per_condition);
        r.explanation = buf;
        return r;
    }

    Agg pa, pb;
    for (const auto* b : A) for (size_t i : b->obs) pa.add(obs[i]);
    for (const auto* b : B) for (size_t i : b->obs) pb.add(obs[i]);
    r.time_ratio = std::exp(pb.mean_y() - pa.mean_y());
    r.hit_rate_diff = pb.hit_rate() - pa.hit_rate();
    r.endpoint_error_ratio = pa.mean_err() > 0 ? pb.mean_err() / pa.mean_err() : 1.0;

    // Two-stage bootstrap: blocks, then trials within blocks.
    Rng rng(opt.seed ^ s.seed);
    std::vector<double> tr, hd, er;
    tr.reserve(size_t(opt.bootstrap_reps));
    for (int i = 0; i < opt.bootstrap_reps; ++i) {
        const Agg a = resample(A, obs, rng);
        const Agg b = resample(B, obs, rng);
        tr.push_back(std::exp(b.mean_y() - a.mean_y()));
        hd.push_back(b.hit_rate() - a.hit_rate());
        er.push_back(a.mean_err() > 0 ? b.mean_err() / a.mean_err() : 1.0);
    }
    r.time_ratio_ci = percentile_interval(tr);
    r.hit_rate_diff_ci = percentile_interval(hd);
    r.endpoint_error_ratio_ci = percentile_interval(er);

    // Block-pair consistency: blocks (0,1), (2,3), … each hold one baseline and one candidate block.
    std::vector<const Block*> ordered;
    for (const auto& kv : blocks)
        if (!kv.second.obs.empty()) ordered.push_back(&kv.second);
    for (size_t i = 0; i + 1 < ordered.size(); i += 2) {
        const Block* x = ordered[i];
        const Block* y = ordered[i + 1];
        if (x->condition == y->condition) continue;
        const Block* base = x->condition == Condition::Baseline ? x : y;
        const Block* cand = x->condition == Condition::Baseline ? y : x;
        Agg ab, ac;
        for (size_t k : base->obs) ab.add(obs[k]);
        for (size_t k : cand->obs) ac.add(obs[k]);
        ++r.block_pairs;
        if (ac.mean_y() < ab.mean_y()) ++r.block_pairs_candidate_faster;
    }

    // Speed must be shown to improve; accuracy must not be shown to get worse, and its point
    // estimate must stay within the margins.
    const bool faster = r.time_ratio_ci.hi < 1.0;
    const bool slower = r.time_ratio_ci.lo > 1.0;
    const bool hit_worse = r.hit_rate_diff_ci.hi < 0.0 && r.hit_rate_diff < -opt.hit_rate_margin / 2.0;
    const bool err_worse = r.endpoint_error_ratio_ci.lo > 1.0;
    const bool hit_ok = !hit_worse && r.hit_rate_diff >= -opt.hit_rate_margin;
    const bool err_ok = !err_worse && r.endpoint_error_ratio <= 1.0 + opt.endpoint_error_margin;
    const bool consistent =
        r.block_pairs > 0 && double(r.block_pairs_candidate_faster) >= opt.min_pair_consistency * double(r.block_pairs);

    char line[512];
    std::snprintf(line, sizeof line,
                  "Aim time (movement onset to hit, adjusted for target distance and direction): candidate/baseline = "
                  "%.3f (95%% range %.3f–%.3f).",
                  r.time_ratio, r.time_ratio_ci.lo, r.time_ratio_ci.hi);
    r.details.push_back(line);
    std::snprintf(line, sizeof line, "Total acquisition time (geometric mean): %.0f ms vs %.0f ms.",
                  r.candidate.geo_mean_acquisition_ms, r.baseline.geo_mean_acquisition_ms);
    r.details.push_back(line);
    std::snprintf(line, sizeof line, "First-click hit rate: %.1f%% vs %.1f%% (difference %+.1f points, range %+.1f to %+.1f).",
                  r.candidate.first_click_hit_rate * 100, r.baseline.first_click_hit_rate * 100, r.hit_rate_diff * 100,
                  r.hit_rate_diff_ci.lo * 100, r.hit_rate_diff_ci.hi * 100);
    r.details.push_back(line);
    std::snprintf(line, sizeof line, "First-flick endpoint error: candidate/baseline = %.3f (range %.3f–%.3f).",
                  r.endpoint_error_ratio, r.endpoint_error_ratio_ci.lo, r.endpoint_error_ratio_ci.hi);
    r.details.push_back(line);
    std::snprintf(line, sizeof line, "Candidate faster in %d of %d block pairs.", r.block_pairs_candidate_faster, r.block_pairs);
    r.details.push_back(line);
    if (excluded) {
        std::snprintf(line, sizeof line, "%d trials excluded (interrupted, anticipated or no flick detected).", excluded);
        r.details.push_back(line);
    }

    if (faster && hit_ok && err_ok && consistent) {
        r.verdict = Verdict::Improved;
        r.recommend_candidate = true;
        r.explanation = "The candidate was faster (" + pct(r.time_ratio) + " shorter aim time, 95% range " +
                        pct(r.time_ratio_ci.hi) + " to " + pct(r.time_ratio_ci.lo) +
                        "), accuracy did not get worse, and the result was consistent across blocks. The candidate is "
                        "recommended.";
    } else if (slower || hit_worse || err_worse) {
        r.verdict = Verdict::Worse;
        if (slower)
            r.explanation = "The candidate made you slower (" + pct(r.time_ratio) + " longer aim time). Keep your baseline.";
        else if (hit_worse)
            r.explanation = "The candidate reduced your first-click accuracy. Keep your baseline.";
        else
            r.explanation = "The candidate made your first flicks land further from the target. Keep your baseline.";
    } else {
        r.verdict = Verdict::NoReliableDifference;
        if (faster && !consistent)
            r.explanation = "The candidate looked faster overall, but not consistently across blocks. That is not reliable "
                            "enough to switch. Keep your baseline (you can repeat the comparison).";
        else if (faster)
            r.explanation = "The candidate was faster, but its accuracy was lower than your baseline's, so it is not "
                            "recommended. Keep your baseline.";
        else if (r.time_ratio < 1.0)
            r.explanation = "The candidate may be slightly faster, but the difference is within run-to-run variation. Keep "
                            "your baseline, or run a longer comparison for a clearer answer.";
        else
            r.explanation = "No reliable difference: the measured change is within run-to-run variation. Keep your baseline.";
    }
    return r;
}

}  // namespace fit
