#include "fit/serialize.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include "fit/timeutil.hpp"

namespace fit {

namespace {

// ---- validated reader -----------------------------------------------------------------------

class Reader {
public:
    Reader(const json* j, std::string path, std::vector<std::string>* errs) : j_(j), path_(std::move(path)), errs_(errs) {}

    bool is_object() const { return j_ && j_->is_object(); }
    const json* raw() const { return j_; }
    const std::string& path() const { return path_; }

    void error(const std::string& field, const std::string& msg) const {
        errs_->push_back((path_.empty() ? field : (field.empty() ? path_ : path_ + "." + field)) + ": " + msg);
    }

    const json* get(const char* key, bool required) const {
        if (!is_object()) return nullptr;
        auto it = j_->find(key);
        if (it == j_->end() || it->is_null()) {
            if (required) error(key, "missing");
            return nullptr;
        }
        return &*it;
    }

    Reader obj(const char* key, bool required = false) const {
        const json* v = get(key, required);
        if (v && !v->is_object()) {
            error(key, "expected an object");
            v = nullptr;
        }
        return Reader(v, sub(key), errs_);
    }

    std::vector<Reader> arr(const char* key, bool required = false) const {
        std::vector<Reader> out;
        const json* v = get(key, required);
        if (!v) return out;
        if (!v->is_array()) {
            error(key, "expected an array");
            return out;
        }
        for (size_t i = 0; i < v->size(); ++i) out.emplace_back(&(*v)[i], sub(key) + "[" + std::to_string(i) + "]", errs_);
        return out;
    }

    const json* array_raw(const char* key, bool required = false) const {
        const json* v = get(key, required);
        if (v && !v->is_array()) {
            error(key, "expected an array");
            return nullptr;
        }
        return v;
    }

    std::string str(const char* key, const std::string& def = {}, bool required = false, size_t max_len = 4096) const {
        const json* v = get(key, required);
        if (!v) return def;
        if (!v->is_string()) {
            error(key, "expected text");
            return def;
        }
        std::string s = v->get<std::string>();
        if (s.size() > max_len) {
            error(key, "text too long");
            return def;
        }
        return s;
    }

    double num(const char* key, double def, double lo, double hi, bool required = false) const {
        const json* v = get(key, required);
        if (!v) return def;
        if (!v->is_number()) {
            error(key, "expected a number");
            return def;
        }
        const double d = v->get<double>();
        if (!std::isfinite(d) || d < lo || d > hi) {
            error(key, "expected a number between " + fmt(lo) + " and " + fmt(hi) + ", got " + fmt(d));
            return def;
        }
        return d;
    }

    // Optional measurement: null/missing -> NaN.
    double meas(const char* key, double lo = -1e12, double hi = 1e12) const {
        const json* v = get(key, false);
        if (!v) return kNaN;
        return num(key, kNaN, lo, hi);
    }

    int i32(const char* key, int def, int lo, int hi, bool required = false) const {
        const double d = num(key, double(def), double(lo), double(hi), required);
        if (d != std::floor(d)) {
            error(key, "expected a whole number");
            return def;
        }
        return int(d);
    }

    int64_t i64(const char* key, int64_t def, bool required = false) const {
        const json* v = get(key, required);
        if (!v) return def;
        if (!v->is_number_integer()) {
            error(key, "expected a whole number");
            return def;
        }
        return v->get<int64_t>();
    }

    bool flag(const char* key, bool def) const {
        const json* v = get(key, false);
        if (!v) return def;
        if (!v->is_boolean()) {
            error(key, "expected true or false");
            return def;
        }
        return v->get<bool>();
    }

    std::map<std::string, std::string> strmap(const char* key) const {
        std::map<std::string, std::string> m;
        Reader o = obj(key);
        if (!o.is_object()) return m;
        for (auto it = o.raw()->begin(); it != o.raw()->end(); ++it) {
            if (it.value().is_string()) m[it.key()] = it.value().get<std::string>();
            else if (it.value().is_number()) m[it.key()] = it.value().dump();
            else if (it.value().is_boolean()) m[it.key()] = it.value().get<bool>() ? "true" : "false";
            else o.error(it.key(), "expected text");
        }
        return m;
    }

    std::string sub(const char* key) const { return path_.empty() ? std::string(key) : path_ + "." + key; }

private:
    static std::string fmt(double d) {
        char b[48];
        std::snprintf(b, sizeof b, "%g", d);
        return b;
    }
    const json* j_;
    std::string path_;
    std::vector<std::string>* errs_;
};

json meas(double v) { return std::isfinite(v) ? json(v) : json(nullptr); }

json strmap_json(const std::map<std::string, std::string>& m) {
    json o = json::object();
    for (const auto& [k, v] : m) o[k] = v;
    return o;
}

template <class E>
E enum_from(const Reader& r, const char* key, std::initializer_list<std::pair<const char*, E>> opts, E def, bool required = false) {
    const std::string s = r.str(key, "", required);
    if (s.empty()) return def;
    for (const auto& [name, val] : opts)
        if (s == name) return val;
    r.error(key, "unknown value \"" + s + "\"");
    return def;
}

bool check_schema(const Reader& r, const char* schema, int max_version, std::vector<std::string>& errors, int* version) {
    const std::string s = r.str("schema", "", true);
    if (s != schema) {
        errors.push_back("This file is not a " + std::string(schema) + " document (schema \"" + s + "\").");
        return false;
    }
    const int v = r.i32("schema_version", 0, 1, 1000000, true);
    if (v > max_version) {
        errors.push_back("This file was created by a newer version of FitSuite (format " + std::to_string(v) +
                         "); this version reads up to format " + std::to_string(max_version) + ".");
        return false;
    }
    if (version) *version = v;
    return v >= 1;
}

}  // namespace

// ---- transforms ---------------------------------------------------------------------------------

json to_json(const Transform& t) {
    json j;
    j["kind"] = to_string(t.kind);
    if (t.kind == TransformKind::Directional) {
        const auto& d = t.directional;
        j["directional"] = {
            {"gain_cos", {d.gain_cos[0], d.gain_cos[1]}},
            {"gain_sin", {d.gain_sin[0], d.gain_sin[1]}},
            {"angle_const", d.angle_const},
            {"angle_cos", {d.angle_cos[0], d.angle_cos[1]}},
            {"angle_sin", {d.angle_sin[0], d.angle_sin[1]}},
            {"strength", d.strength},
        };
    } else if (t.kind == TransformKind::RawAccel) {
        const auto& r = t.raw_accel;
        j["raw_accel"] = {{"rotation_deg", r.rotation_deg}, {"sens", r.sens}, {"yx_ratio", r.yx_ratio},
                          {"lr_ratio", r.lr_ratio},         {"ud_ratio", r.ud_ratio}};
    }
    return j;
}

bool transform_within_limits(const Transform& t, std::string* why) {
    auto fail = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (t.kind == TransformKind::Directional) {
        const auto& d = t.directional;
        if (!(d.strength >= 0.0 && d.strength <= 1.0)) return fail("strength must be between 0 and 1");
        DirectionalProfile full = d;
        full.strength = 1.0;
        if (full.max_gain_deviation() > kHardMaxGainDeviation + 1e-9)
            return fail("gain changes by more than the 15% safety limit");
        if (full.max_angle_deg() > kHardMaxAngleDeg + 1e-9) return fail("angle changes by more than the 6° safety limit");
    } else if (t.kind == TransformKind::RawAccel) {
        const auto& r = t.raw_accel;
        const double lo = 1.0 / (1.0 + kHardMaxGainDeviation), hi = 1.0 + kHardMaxGainDeviation;
        if (std::fabs(r.rotation_deg) > kHardMaxAngleDeg) return fail("rotation exceeds the 6° safety limit");
        for (double v : {r.yx_ratio, r.lr_ratio, r.ud_ratio})
            if (!(v >= lo && v <= hi)) return fail("a ratio exceeds the 15% safety limit");
        if (!(r.sens >= lo && r.sens <= hi)) return fail("sensitivity multiplier exceeds the 15% safety limit");
    }
    return true;
}

std::optional<Transform> transform_from_json(const json& j, const std::string& path, std::vector<std::string>& errors) {
    const size_t before = errors.size();
    Reader r(&j, path, &errors);
    if (!r.is_object()) {
        errors.push_back(path + ": expected an object");
        return std::nullopt;
    }
    Transform t;
    t.kind = enum_from<TransformKind>(r, "kind",
                                      {{"identity", TransformKind::Identity},
                                       {"directional", TransformKind::Directional},
                                       {"rawaccel", TransformKind::RawAccel}},
                                      TransformKind::Identity, true);
    auto pair = [&](const Reader& o, const char* key, std::array<double, 2>& out) {
        const json* a = o.array_raw(key, true);
        if (!a) return;
        if (a->size() != 2 || !(*a)[0].is_number() || !(*a)[1].is_number()) {
            o.error(key, "expected two numbers");
            return;
        }
        out[0] = (*a)[0].get<double>();
        out[1] = (*a)[1].get<double>();
        if (!std::isfinite(out[0]) || !std::isfinite(out[1]) || std::fabs(out[0]) > 1 || std::fabs(out[1]) > 1)
            o.error(key, "coefficient out of range");
    };
    if (t.kind == TransformKind::Directional) {
        Reader d = r.obj("directional", true);
        if (d.is_object()) {
            pair(d, "gain_cos", t.directional.gain_cos);
            pair(d, "gain_sin", t.directional.gain_sin);
            t.directional.angle_const = d.num("angle_const", 0.0, -1.0, 1.0, true);
            pair(d, "angle_cos", t.directional.angle_cos);
            pair(d, "angle_sin", t.directional.angle_sin);
            t.directional.strength = d.num("strength", 1.0, 0.0, 1.0, true);
        }
    } else if (t.kind == TransformKind::RawAccel) {
        Reader a = r.obj("raw_accel", true);
        if (a.is_object()) {
            t.raw_accel.rotation_deg = a.num("rotation_deg", 0.0, -180.0, 180.0, true);
            t.raw_accel.sens = a.num("sens", 1.0, 0.01, 100.0, true);
            t.raw_accel.yx_ratio = a.num("yx_ratio", 1.0, 0.01, 100.0, true);
            t.raw_accel.lr_ratio = a.num("lr_ratio", 1.0, 0.01, 100.0, true);
            t.raw_accel.ud_ratio = a.num("ud_ratio", 1.0, 0.01, 100.0, true);
        }
    }
    if (errors.size() != before) return std::nullopt;
    std::string why;
    if (!transform_within_limits(t, &why)) {
        errors.push_back(path + ": rejected because the " + why + ".");
        return std::nullopt;
    }
    return t;
}

// ---- mouse profile records ----------------------------------------------------------------------

json to_json(const MouseProfileRecord& m) {
    json j;
    j["id"] = m.id;
    j["label"] = m.label;
    j["created_utc"] = m.created_utc;
    j["algorithm_version"] = m.algorithm_version;
    j["calibration_session"] = m.calibration_session;
    j["status"] = m.status;
    j["origin"] = m.origin;
    j["transform"] = to_json(m.transform);
    j["conditions"] = strmap_json(m.conditions);
    if (m.validation) {
        const auto& v = *m.validation;
        j["validation"] = {{"session_id", v.session_id}, {"finished_utc", v.finished_utc}, {"verdict", v.verdict},
                           {"explanation", v.explanation}, {"time_ratio", v.time_ratio}, {"time_ratio_lo", v.time_ratio_lo},
                           {"time_ratio_hi", v.time_ratio_hi}, {"hit_rate_diff", v.hit_rate_diff}, {"strength", v.strength}};
    }
    if (m.last_check) {
        const auto& c = *m.last_check;
        j["last_check"] = {{"session_id", c.session_id}, {"finished_utc", c.finished_utc}, {"verdict", c.verdict},
                           {"explanation", c.explanation}, {"max_residual_pct", c.max_residual_pct}};
    }
    return j;
}

static std::optional<MouseProfileRecord> mouse_profile_from(const Reader& r, std::vector<std::string>& errors) {
    const size_t before = errors.size();
    MouseProfileRecord m;
    m.id = r.str("id", "", true, 128);
    m.label = r.str("label", "Profile", false, 200);
    m.created_utc = r.str("created_utc");
    m.algorithm_version = r.str("algorithm_version", "", true, 64);
    m.calibration_session = r.str("calibration_session");
    m.status = r.str("status", "candidate");
    if (m.status != "candidate" && m.status != "active" && m.status != "discarded") {
        r.error("status", "unknown value");
        m.status = "candidate";
    }
    m.origin = r.str("origin", "measured");
    if (m.origin != "measured" && m.origin != "simulated") r.error("origin", "unknown value");
    if (const json* t = r.get("transform", true)) {
        auto tr = transform_from_json(*t, r.sub("transform"), errors);
        if (tr) m.transform = *tr;
    }
    m.conditions = r.strmap("conditions");
    Reader v = r.obj("validation");
    if (v.is_object()) {
        ValidationSummary s;
        s.session_id = v.str("session_id");
        s.finished_utc = v.str("finished_utc");
        s.verdict = v.str("verdict");
        s.explanation = v.str("explanation");
        s.time_ratio = v.num("time_ratio", 1.0, 0.01, 100.0);
        s.time_ratio_lo = v.num("time_ratio_lo", 1.0, 0.01, 100.0);
        s.time_ratio_hi = v.num("time_ratio_hi", 1.0, 0.01, 100.0);
        s.hit_rate_diff = v.num("hit_rate_diff", 0.0, -1.0, 1.0);
        s.strength = v.num("strength", 1.0, 0.0, 1.0);
        m.validation = s;
    }
    Reader lc = r.obj("last_check");  // optional: absent in files from before the quick check
    if (lc.is_object()) {
        CheckSummary c;
        c.session_id = lc.str("session_id");
        c.finished_utc = lc.str("finished_utc");
        c.verdict = lc.str("verdict", "not_enough_data");
        if (c.verdict != "still_fits" && c.verdict != "changed" && c.verdict != "not_enough_data") {
            lc.error("verdict", "unknown value");
            c.verdict = "not_enough_data";
        }
        c.explanation = lc.str("explanation");
        c.max_residual_pct = lc.num("max_residual_pct", 0.0, 0.0, 1000.0);
        m.last_check = c;
    }
    if (m.algorithm_version.rfind("mousefit-", 0) != 0 && errors.size() == before)
        r.error("algorithm_version", "unknown algorithm \"" + m.algorithm_version + "\"");
    if (errors.size() != before) return std::nullopt;
    return m;
}

json export_mouse_profile(const MouseProfileRecord& r) {
    return {{"schema", kMouseProfileSchema}, {"schema_version", kMouseProfileSchemaVersion}, {"exported_utc", utc_now_iso()},
            {"app_version", kAppVersion}, {"profile", to_json(r)}};
}

std::optional<MouseProfileRecord> import_mouse_profile(const json& j, std::vector<std::string>& errors) {
    Reader r(&j, "", &errors);
    if (!r.is_object()) {
        errors.push_back("The file is not a JSON object.");
        return std::nullopt;
    }
    if (!check_schema(r, kMouseProfileSchema, kMouseProfileSchemaVersion, errors, nullptr)) return std::nullopt;
    Reader p = r.obj("profile", true);
    if (!p.is_object()) return std::nullopt;
    auto m = mouse_profile_from(p, errors);
    if (m) {
        // An imported profile is never active and its validation belongs to another install.
        m->status = "candidate";
    }
    return m;
}

// ---- setup profile ------------------------------------------------------------------------------

json to_json(const SetupProfile& p) {
    json j;
    j["schema"] = kSetupSchema;
    j["schema_version"] = p.schema_version;
    j["app_version"] = p.app_version;
    j["id"] = p.id;
    j["name"] = p.name;
    j["created_utc"] = p.created_utc;
    j["updated_utc"] = p.updated_utc;

    json gpus = json::array();
    for (const auto& g : p.pc.gpus)
        gpus.push_back({{"name", g.name}, {"driver", g.driver}, {"vendor_id", g.vendor_id}, {"device_id", g.device_id},
                        {"vram_gb", g.vram_gb}});
    j["hardware"] = {
        {"mouse", {{"model", p.mouse.model}, {"name", p.mouse.name}, {"device_id", p.mouse.device_id},
                   {"vid_pid", p.mouse.vid_pid}, {"dpi", p.mouse.dpi}, {"polling_hz", p.mouse.polling_hz},
                   {"source", p.mouse.source}}},
        {"pc", {{"cpu", p.pc.cpu}, {"os", p.pc.os}, {"ram_gb", p.pc.ram_gb}, {"gpus", gpus}, {"source", p.pc.source}}},
        {"display", {{"name", p.display.name}, {"device", p.display.device}, {"edid_id", p.display.edid_id},
                     {"width", p.display.width}, {"height", p.display.height}, {"refresh_hz", p.display.refresh_hz},
                     {"modes_hz", p.display.modes_hz}, {"overdrive", p.display.overdrive}, {"ddcci", p.display.ddcci},
                     {"source", p.display.source}}},
    };
    j["game"] = {{"id", p.game.id}, {"sensitivity", p.game.sensitivity}, {"cm360", p.game.cm360},
                 {"scoped_multiplier", p.game.scoped_multiplier},
                 {"fps_cap", p.game.fps_cap}, {"graphics", strmap_json(p.game.graphics)}};
    j["baseline"] = {{"captured_utc", p.baseline_captured_utc}, {"settings", strmap_json(p.baseline)}};

    json mp = json::array();
    for (const auto& m : p.mouse_profiles) mp.push_back(to_json(m));
    j["mousefit"] = {{"profiles", mp}, {"active_profile", p.active_mouse_profile}, {"sessions", p.mouse_sessions}};

    json fc = json::array();
    for (const auto& c : p.frame_configs) {
        json runs = json::array();
        for (const auto& r : c.runs)
            runs.push_back({{"id", r.id}, {"timestamp_utc", r.timestamp_utc}, {"csv_file", r.csv_file}, {"origin", r.origin},
                            {"application", r.application}, {"source_format", r.source_format},
                            {"present_mode", r.present_mode}, {"duration_s", r.duration_s},
                            {"avg_fps", meas(r.metrics.avg_fps)}, {"low1_fps", meas(r.metrics.low1_fps)},
                            {"p99_ms", meas(r.metrics.p99_ms)}, {"stutters_per_min", meas(r.metrics.stutters_per_min)},
                            {"latency_p50_ms", meas(r.metrics.latency_p50_ms)},
                            {"latency_definition", r.metrics.latency_definition}, {"ft_p50_ms", meas(r.ft_p50_ms)},
                            {"ft_p95_ms", meas(r.ft_p95_ms)}, {"low01_fps", meas(r.low01_fps)},
                            {"dropped_pct", meas(r.dropped_pct)}, {"cpu_busy_p50_ms", meas(r.cpu_busy_p50_ms)},
                            {"gpu_busy_p50_ms", meas(r.gpu_busy_p50_ms)}, {"bound_hint", r.bound_hint},
                            {"conditions", strmap_json(r.conditions)}});
        fc.push_back({{"id", c.id}, {"label", c.label}, {"baseline", c.baseline}, {"settings", strmap_json(c.settings)},
                      {"user_confirmed", c.user_confirmed}, {"runs", runs}});
    }
    j["framefit"] = {{"configs", fc}};

    json ms = json::array();
    for (const auto& s : p.motion_sessions) {
        json cfgs = json::array(), resp = json::array(), rat = json::array();
        for (const auto& c : s.configs)
            cfgs.push_back({{"label", c.label}, {"refresh_hz", c.refresh_hz}, {"overdrive", c.overdrive},
                            {"content_fps", c.content_fps}, {"app_controlled", c.app_controlled}});
        for (const auto& r : s.responses)
            resp.push_back({r.spec.index, r.spec.config, r.spec.block, r.spec.speed_sw_s, r.spec.orientation, r.answer,
                            r.correct ? 1 : 0, r.response_ms, r.dropped_frames});
        for (const auto& r : s.ratings) rat.push_back({r.config, r.round, r.trail, r.overshoot});
        ms.push_back({{"id", s.id}, {"timestamp_utc", s.timestamp_utc}, {"origin", s.origin}, {"test", s.test},
                      {"configs", cfgs}, {"responses", resp}, {"ratings", rat}, {"conditions", strmap_json(s.conditions)}});
    }
    j["motionfit"] = {{"sessions", ms}, {"selected", p.motion_selected}};
    j["fix_games"] = {{"enabled", p.fix_games.enabled}, {"games", p.fix_games.games},
                      {"rawaccel_settings", p.fix_games.rawaccel_settings},
                      {"rawaccel_corrected", p.fix_games.rawaccel_corrected},
                      {"start_with_windows", p.fix_games.start_with_windows}};
    json gs = json::object();
    for (const auto& [exe, v] : p.app.game_sens) gs[exe] = {{"sensitivity", v.sensitivity}, {"cm360", v.cm360}};
    j["app"] = {{"theme", p.app.theme},
                {"start_with_windows", p.app.start_with_windows},
                {"start_hidden", p.app.start_hidden},
                {"close_to_tray", p.app.close_to_tray},
                {"show_extra_tools", p.app.show_extra_tools},
                {"check_reminder_days", p.app.check_reminder_days},
                {"last_reminder_utc", p.app.last_reminder_utc},
                {"game_sensitivity", gs},
                {"refresh_kept", p.app.refresh_kept},
                {"setup_done", p.app.setup_done},
                {"setup_step", p.app.setup_step},
                {"rawaccel_folder", p.app.rawaccel_folder},
                {"rawaccel_install_pending", p.app.rawaccel_install_pending},
                {"update_check", p.app.update_check},
                {"last_update_check_utc", p.app.last_update_check_utc},
                {"update_notified", p.app.update_notified},
                {"notes_seen", p.app.notes_seen}};

    json rs = json::array();
    for (const auto& r : p.results)
        rs.push_back({{"id", r.id}, {"tool", r.tool}, {"kind", r.kind}, {"timestamp_utc", r.timestamp_utc},
                      {"summary", r.summary}, {"origin", r.origin}, {"conditions", strmap_json(r.conditions)}});
    j["results"] = rs;

    json ac = json::array();
    for (const auto& a : p.applied_changes)
        ac.push_back({{"id", a.id}, {"kind", a.kind}, {"target", a.target}, {"before", a.before}, {"after", a.after},
                      {"applied_utc", a.applied_utc}, {"reverted_utc", a.reverted_utc}, {"status", a.status},
                      {"temporary", a.temporary}});
    j["applied_changes"] = ac;
    return j;
}

std::optional<SetupProfile> setup_from_json(const json& j, std::vector<std::string>& errors) {
    const size_t before = errors.size();
    Reader r(&j, "", &errors);
    if (!r.is_object()) {
        errors.push_back("The setup file is not a JSON object.");
        return std::nullopt;
    }
    int version = 0;
    if (!check_schema(r, kSetupSchema, kSetupSchemaVersion, errors, &version)) return std::nullopt;
    SetupProfile p;
    p.schema_version = kSetupSchemaVersion;
    p.app_version = r.str("app_version");
    p.id = r.str("id", "", true, 128);
    p.name = r.str("name", "My setup", false, 200);
    p.created_utc = r.str("created_utc");
    p.updated_utc = r.str("updated_utc");

    Reader hw = r.obj("hardware", true);
    Reader mo = hw.obj("mouse");
    p.mouse.model = mo.str("model", "", false, 120);
    p.mouse.name = mo.str("name");
    p.mouse.device_id = mo.str("device_id");
    p.mouse.vid_pid = mo.str("vid_pid");
    p.mouse.dpi = mo.i32("dpi", 800, 50, 64000);
    p.mouse.polling_hz = mo.num("polling_hz", 0.0, 0.0, 100000.0);
    p.mouse.source = mo.str("source", "user");
    Reader pc = hw.obj("pc");
    p.pc.cpu = pc.str("cpu");
    p.pc.os = pc.str("os");
    p.pc.ram_gb = pc.num("ram_gb", 0.0, 0.0, 1e6);
    p.pc.source = pc.str("source", "not detected");
    for (const auto& g : pc.arr("gpus")) {
        GpuInfo gi;
        gi.name = g.str("name");
        gi.driver = g.str("driver");
        gi.vendor_id = uint32_t(g.num("vendor_id", 0, 0, 4294967295.0));
        gi.device_id = uint32_t(g.num("device_id", 0, 0, 4294967295.0));
        gi.vram_gb = g.num("vram_gb", 0, 0, 1e6);
        p.pc.gpus.push_back(gi);
    }
    Reader di = hw.obj("display");
    p.display.name = di.str("name");
    p.display.device = di.str("device");
    p.display.edid_id = di.str("edid_id");
    p.display.width = di.i32("width", 0, 0, 100000);
    p.display.height = di.i32("height", 0, 0, 100000);
    p.display.refresh_hz = di.num("refresh_hz", 0.0, 0.0, 2000.0);
    if (const json* m = di.array_raw("modes_hz"))
        for (const auto& v : *m)
            if (v.is_number() && std::isfinite(v.get<double>()) && v.get<double>() > 0 && v.get<double>() < 2000)
                p.display.modes_hz.push_back(v.get<double>());
    p.display.overdrive = di.str("overdrive");
    p.display.ddcci = di.str("ddcci", "unknown");
    p.display.source = di.str("source", "not detected");

    Reader g = r.obj("game", true);
    p.game.id = g.str("id", "valorant");
    p.game.sensitivity = g.num("sensitivity", 0.4, 0.0001, 100.0);
    p.game.cm360 = g.num("cm360", 0.0, 0.0, 10000.0);
    p.game.scoped_multiplier = g.num("scoped_multiplier", 1.0, 0.01, 10.0);
    p.game.fps_cap = g.i32("fps_cap", 0, 0, 10000);
    p.game.graphics = g.strmap("graphics");

    Reader b = r.obj("baseline");
    p.baseline_captured_utc = b.str("captured_utc");
    p.baseline = b.strmap("settings");

    Reader mf = r.obj("mousefit");
    for (const auto& m : mf.arr("profiles"))
        if (auto rec = mouse_profile_from(m, errors)) p.mouse_profiles.push_back(*rec);
    p.active_mouse_profile = mf.str("active_profile");
    if (const json* ss = mf.array_raw("sessions"))
        for (const auto& v : *ss)
            if (v.is_string()) p.mouse_sessions.push_back(v.get<std::string>());

    Reader ff = r.obj("framefit");
    for (const auto& c : ff.arr("configs")) {
        FrameConfig fc;
        fc.id = c.str("id", "", true);
        fc.label = c.str("label", "Configuration");
        fc.baseline = c.flag("baseline", false);
        fc.settings = c.strmap("settings");
        fc.user_confirmed = c.flag("user_confirmed", false);
        for (const auto& rr : c.arr("runs")) {
            FrameRun run;
            run.id = rr.str("id", "", true);
            run.timestamp_utc = rr.str("timestamp_utc");
            run.csv_file = rr.str("csv_file");
            run.origin = rr.str("origin", "measured");
            run.application = rr.str("application");
            run.source_format = rr.str("source_format");
            run.present_mode = rr.str("present_mode");
            run.duration_s = rr.num("duration_s", 0.0, 0.0, 1e7);
            run.metrics.avg_fps = rr.meas("avg_fps", 0, 100000);
            run.metrics.low1_fps = rr.meas("low1_fps", 0, 100000);
            run.metrics.p99_ms = rr.meas("p99_ms", 0, 100000);
            run.metrics.stutters_per_min = rr.meas("stutters_per_min", 0, 1e7);
            run.metrics.latency_p50_ms = rr.meas("latency_p50_ms", 0, 100000);
            run.metrics.latency_definition = rr.str("latency_definition");
            run.ft_p50_ms = rr.meas("ft_p50_ms", 0, 100000);
            run.ft_p95_ms = rr.meas("ft_p95_ms", 0, 100000);
            run.low01_fps = rr.meas("low01_fps", 0, 100000);
            run.dropped_pct = rr.meas("dropped_pct", 0, 100);
            run.cpu_busy_p50_ms = rr.meas("cpu_busy_p50_ms", 0, 100000);
            run.gpu_busy_p50_ms = rr.meas("gpu_busy_p50_ms", 0, 100000);
            run.bound_hint = rr.str("bound_hint");
            run.conditions = rr.strmap("conditions");
            fc.runs.push_back(run);
        }
        p.frame_configs.push_back(fc);
    }

    Reader mm = r.obj("motionfit");
    for (const auto& s : mm.arr("sessions")) {
        MotionSessionRecord ms;
        ms.id = s.str("id", "", true);
        ms.timestamp_utc = s.str("timestamp_utc");
        ms.origin = s.str("origin", "measured");
        ms.test = s.str("test", "identification");
        for (const auto& c : s.arr("configs")) {
            MotionConfig mc;
            mc.label = c.str("label");
            mc.refresh_hz = c.num("refresh_hz", 0, 0, 2000);
            mc.overdrive = c.str("overdrive");
            mc.content_fps = c.num("content_fps", 0, 0, 2000);
            mc.app_controlled = c.flag("app_controlled", false);
            ms.configs.push_back(mc);
        }
        if (const json* rs = s.array_raw("responses"))
            for (size_t i = 0; i < rs->size(); ++i) {
                const json& a = (*rs)[i];
                bool ok = a.is_array() && a.size() == 9;
                for (size_t k = 0; ok && k < 9; ++k) ok = a[k].is_number();
                if (!ok) {
                    errors.push_back(s.sub("responses") + "[" + std::to_string(i) + "]: expected 9 numbers");
                    continue;
                }
                MotionResponse mr;
                mr.spec.index = a[0].get<int>();
                mr.spec.config = a[1].get<int>();
                mr.spec.block = a[2].get<int>();
                mr.spec.speed_sw_s = a[3].get<double>();
                mr.spec.orientation = a[4].get<int>();
                mr.answer = a[5].get<int>();
                mr.correct = a[6].get<int>() != 0;
                mr.response_ms = a[7].get<double>();
                mr.dropped_frames = a[8].get<int>();
                if (mr.spec.config < 0 || mr.spec.config >= int(ms.configs.size()) || !(mr.spec.speed_sw_s > 0)) {
                    errors.push_back(s.sub("responses") + "[" + std::to_string(i) + "]: invalid configuration or speed");
                    continue;
                }
                ms.responses.push_back(mr);
            }
        if (const json* rt = s.array_raw("ratings"))
            for (size_t i = 0; i < rt->size(); ++i) {
                const json& a = (*rt)[i];
                if (!a.is_array() || a.size() != 4 || !a[0].is_number() || !a[1].is_number() || !a[2].is_number() ||
                    !a[3].is_number()) {
                    errors.push_back(s.sub("ratings") + "[" + std::to_string(i) + "]: expected 4 numbers");
                    continue;
                }
                TrailRating tr{a[0].get<int>(), a[1].get<int>(), a[2].get<int>(), a[3].get<int>()};
                if (tr.trail < 0 || tr.trail > 4 || tr.overshoot < 0 || tr.overshoot > 4) {
                    errors.push_back(s.sub("ratings") + "[" + std::to_string(i) + "]: rating outside 0–4");
                    continue;
                }
                ms.ratings.push_back(tr);
            }
        ms.conditions = s.strmap("conditions");
        p.motion_sessions.push_back(ms);
    }
    p.motion_selected = mm.str("selected");
    {
        const Reader fg = r.obj("fix_games");  // optional: absent in files from before this feature
        p.fix_games.enabled = fg.flag("enabled", false);
        p.fix_games.rawaccel_settings = fg.str("rawaccel_settings");
        p.fix_games.rawaccel_corrected = fg.str("rawaccel_corrected");
        p.fix_games.start_with_windows = fg.flag("start_with_windows", true);
        if (const json* g = fg.array_raw("games")) {
            p.fix_games.games.clear();  // the stored list replaces the default
            for (size_t i = 0; i < g->size(); ++i) {
                if (!(*g)[i].is_string() || (*g)[i].get<std::string>().size() > 260) {
                    errors.push_back(fg.sub("games") + "[" + std::to_string(i) + "]: expected a program name");
                    continue;
                }
                p.fix_games.games.push_back((*g)[i].get<std::string>());
            }
        }
        if (p.fix_games.enabled && (p.fix_games.rawaccel_settings.empty() || p.fix_games.rawaccel_corrected.empty()))
            p.fix_games.enabled = false;  // nothing to switch between
    }

    Reader ap = r.obj("app");  // optional: absent in files from before the Settings tab
    if (ap.is_object()) {
        p.app.theme = ap.str("theme", "system", false, 16);
        if (p.app.theme != "system" && p.app.theme != "light" && p.app.theme != "dark") p.app.theme = "system";
        p.app.start_with_windows = ap.flag("start_with_windows", false);
        p.app.start_hidden = ap.flag("start_hidden", true);
        p.app.close_to_tray = ap.flag("close_to_tray", true);
        p.app.show_extra_tools = ap.flag("show_extra_tools", false);
        p.app.check_reminder_days = ap.i32("check_reminder_days", 7, 0, 365);
        p.app.last_reminder_utc = ap.str("last_reminder_utc", "", false, 64);
        Reader gs = ap.obj("game_sensitivity");
        if (gs.is_object())
            for (auto it = gs.raw()->begin(); it != gs.raw()->end(); ++it) {
                if (it.key().empty() || it.key().size() > 260) continue;
                Reader e(&it.value(), gs.sub(it.key().c_str()), &errors);
                GameSensitivity v;
                v.sensitivity = e.num("sensitivity", 0.0, 0.0, 10000.0);
                v.cm360 = e.num("cm360", 0.0, 0.0, 10000.0);
                p.app.game_sens[it.key()] = v;
            }
        // Files from before the setup: players who already measured skip it; everyone else gets it.
        p.app.setup_done = ap.flag("setup_done", !p.mouse_sessions.empty());
        p.app.setup_step = ap.i32("setup_step", 0, 0, 20);
        p.app.rawaccel_folder = ap.str("rawaccel_folder", "", false, 1024);
        p.app.rawaccel_install_pending = ap.flag("rawaccel_install_pending", false);
        p.app.update_check = ap.flag("update_check", true);
        p.app.last_update_check_utc = ap.str("last_update_check_utc", "", false, 64);
        p.app.update_notified = ap.str("update_notified", "", false, 64);
        // Files from earlier versions have none: their players see the new version's notes once.
        p.app.notes_seen = ap.str("notes_seen", "", false, 64);
        if (const json* rk = ap.array_raw("refresh_kept"))
            for (const auto& e : *rk)
                if (e.is_string() && !e.get<std::string>().empty() && e.get<std::string>().size() <= 300 &&
                    p.app.refresh_kept.size() < 64)
                    p.app.refresh_kept.push_back(e.get<std::string>());
    } else {
        // Older files: "Start with Windows" belonged to the per-game fix.
        p.app.start_with_windows = p.fix_games.enabled && p.fix_games.start_with_windows;
        p.app.start_hidden = true;
        p.app.setup_done = !p.mouse_sessions.empty();
    }

    for (const auto& rr : r.arr("results")) {
        ResultRecord rec;
        rec.id = rr.str("id", "", true);
        rec.tool = rr.str("tool", "", true);
        rec.kind = rr.str("kind");
        rec.timestamp_utc = rr.str("timestamp_utc");
        rec.summary = rr.str("summary");
        rec.origin = rr.str("origin", "measured");
        rec.conditions = rr.strmap("conditions");
        p.results.push_back(rec);
    }
    for (const auto& a : r.arr("applied_changes")) {
        AppliedChange c;
        c.id = a.str("id", "", true);
        c.kind = a.str("kind", "", true);
        c.target = a.str("target");
        c.before = a.str("before");
        c.after = a.str("after");
        c.applied_utc = a.str("applied_utc");
        c.reverted_utc = a.str("reverted_utc");
        c.status = a.str("status", "applied");
        c.temporary = a.flag("temporary", false);
        p.applied_changes.push_back(c);
    }
    if (!p.active_mouse_profile.empty()) {
        bool found = false;
        for (const auto& m : p.mouse_profiles) found = found || m.id == p.active_mouse_profile;
        if (!found) errors.push_back("mousefit.active_profile: refers to a profile that does not exist");
    }
    if (errors.size() != before) return std::nullopt;
    return p;
}

// ---- sessions -----------------------------------------------------------------------------------

json to_json(const Session& s) {
    json j;
    j["schema"] = kSessionSchema;
    j["schema_version"] = kSessionSchemaVersion;
    j["id"] = s.id;
    j["kind"] = to_string(s.kind);
    j["started_utc"] = s.started_utc;
    j["finished_utc"] = s.finished_utc;
    j["complete"] = s.complete;
    j["algorithm_version"] = s.algorithm_version;
    j["seed"] = s.seed;
    j["candidate_profile_id"] = s.candidate_profile_id;
    j["format"] = s.format;
    const auto& c = s.conditions;
    j["conditions"] = {{"mouse_name", c.mouse_name}, {"mouse_id", c.mouse_id}, {"mouse_vid_pid", c.mouse_vid_pid},
                       {"dpi", c.dpi}, {"polling_hz_measured", c.polling_hz_measured},
                       {"game_id", c.mapping.game_id}, {"sensitivity", c.mapping.sensitivity},
                       {"deg_per_count_at_sens1", c.mapping.deg_per_count_at_sens1}, {"hfov_deg_16x9", c.mapping.hfov_deg_16x9},
                       {"display_refresh_hz", c.display_refresh_hz}, {"display_width", c.display_width},
                       {"display_height", c.display_height}, {"render_fps_median", c.render_fps_median},
                       {"render_frame_p99_ms", c.render_frame_p99_ms}, {"render_hitches", c.render_hitches},
                       {"tearing_allowed", c.tearing_allowed}, {"data_origin", c.data_origin}};
    json trials = json::array();
    for (const auto& t : s.trials) {
        json motion = json::array();
        for (const auto& m : t.motion) {
            motion.push_back(m.t_us - t.t_appear_us);
            motion.push_back(m.in_dx);
            motion.push_back(m.in_dy);
            motion.push_back(m.out_dx);
            motion.push_back(m.out_dy);
        }
        json clicks = json::array();
        for (const auto& k : t.clicks) clicks.push_back({k.t_us - t.t_appear_us, k.hit ? 1 : 0, k.offset_deg});
        trials.push_back({{"spec", {{"dir_deg", t.spec.dir_deg}, {"dist_deg", t.spec.dist_deg},
                                    {"radius_deg", t.spec.radius_deg}, {"block", t.spec.block}, {"index", t.spec.index},
                                    {"condition", to_string(t.spec.condition)}, {"purpose", to_string(t.spec.purpose)},
                                    {"task", to_string(t.spec.task)}, {"to_anchor", t.spec.to_anchor},
                                    {"nominal_dist_deg", t.spec.nominal_dist_deg}}},
                          {"timeout_ms", t.timeout_ms},
                          {"target", {t.target_counts.x, t.target_counts.y}},
                          {"start", {t.start_yaw_deg, t.start_pitch_deg}},
                          {"t_appear_us", t.t_appear_us},
                          {"duration_us", t.t_end_us - t.t_appear_us},
                          {"outcome", to_string(t.outcome)},
                          {"motion", motion},
                          {"clicks", clicks}});
    }
    j["trials"] = trials;
    json rounds = json::array();
    for (const auto& rr : s.rounds)
        rounds.push_back({{"index", rr.index}, {"task", to_string(rr.task)}, {"condition", to_string(rr.condition)},
                          {"t_start_us", rr.t_start_us}, {"t_end_us", rr.t_end_us}, {"hits", rr.hits},
                          {"misses", rr.misses}, {"timeouts", rr.timeouts}, {"complete", rr.complete}});
    j["rounds"] = rounds;
    // Tracking: flat groups of 9 numbers per frame (time relative to the session's first sample).
    json track = json::array();
    const int64_t t0 = s.tracking.empty() ? 0 : s.tracking.front().t_us;
    for (const auto& k : s.tracking) {
        track.push_back(k.t_us - t0);
        track.push_back(k.round);
        for (float v : {k.target_yaw, k.target_pitch, k.aim_yaw, k.aim_pitch, k.target_vyaw, k.target_vpitch, k.radius})
            track.push_back(std::round(double(v) * 1000.0) / 1000.0);
    }
    j["tracking_t0_us"] = t0;
    j["tracking"] = track;
    return j;
}

std::optional<Session> session_from_json(const json& j, std::vector<std::string>& errors) {
    const size_t before = errors.size();
    Reader r(&j, "", &errors);
    if (!r.is_object()) {
        errors.push_back("The session file is not a JSON object.");
        return std::nullopt;
    }
    if (!check_schema(r, kSessionSchema, kSessionSchemaVersion, errors, nullptr)) return std::nullopt;
    Session s;
    s.id = r.str("id", "", true, 128);
    s.kind = enum_from<SessionKind>(r, "kind",
                                    {{"calibration", SessionKind::Calibration},
                                     {"validation", SessionKind::Validation},
                                     {"check", SessionKind::Check}},
                                    SessionKind::Calibration, true);
    s.started_utc = r.str("started_utc");
    s.finished_utc = r.str("finished_utc");
    s.complete = r.flag("complete", false);
    s.algorithm_version = r.str("algorithm_version", "", true);
    if (const json* sd = r.get("seed", false)) {
        if (sd->is_number_unsigned() || sd->is_number_integer()) s.seed = sd->get<uint64_t>();
        else r.error("seed", "expected a whole number");
    }
    s.candidate_profile_id = r.str("candidate_profile_id");
    s.format = r.i32("format", 1, 1, 2);
    Reader c = r.obj("conditions", true);
    auto& sc = s.conditions;
    sc.mouse_name = c.str("mouse_name");
    sc.mouse_id = c.str("mouse_id");
    sc.mouse_vid_pid = c.str("mouse_vid_pid");
    sc.dpi = c.i32("dpi", 800, 50, 64000);
    sc.polling_hz_measured = c.num("polling_hz_measured", 0, 0, 100000);
    sc.mapping.game_id = c.str("game_id", "valorant");
    sc.mapping.sensitivity = c.num("sensitivity", 0.4, 0.0001, 100.0, true);
    sc.mapping.deg_per_count_at_sens1 = c.num("deg_per_count_at_sens1", 0.07, 1e-6, 10.0);
    sc.mapping.hfov_deg_16x9 = c.num("hfov_deg_16x9", 103.0, 10.0, 170.0);
    sc.display_refresh_hz = c.num("display_refresh_hz", 0, 0, 2000);
    sc.display_width = c.i32("display_width", 0, 0, 100000);
    sc.display_height = c.i32("display_height", 0, 0, 100000);
    sc.render_fps_median = c.num("render_fps_median", 0, 0, 100000);
    sc.render_frame_p99_ms = c.num("render_frame_p99_ms", 0, 0, 100000);
    sc.render_hitches = c.i32("render_hitches", 0, 0, 100000000);
    sc.tearing_allowed = c.flag("tearing_allowed", false);
    sc.data_origin = c.str("data_origin", "measured");
    if (sc.data_origin != "measured" && sc.data_origin != "simulated") c.error("data_origin", "unknown value");

    for (const auto& t : r.arr("trials", true)) {
        Trial tr;
        Reader sp = t.obj("spec", true);
        tr.spec.dir_deg = sp.num("dir_deg", 0, -720, 720, true);
        tr.spec.dist_deg = sp.num("dist_deg", 10, 0.01, 180, true);
        tr.spec.radius_deg = sp.num("radius_deg", 1.2, 0.01, 45, true);
        tr.spec.block = sp.i32("block", 0, -1, 100000);
        tr.spec.index = sp.i32("index", 0, -100000000, 100000000);
        tr.spec.condition = enum_from<Condition>(sp, "condition", {{"baseline", Condition::Baseline}, {"candidate", Condition::Candidate}},
                                                 Condition::Baseline, true);
        tr.spec.purpose = enum_from<TrialPurpose>(
            sp, "purpose",
            {{"scored", TrialPurpose::Scored}, {"recenter", TrialPurpose::Recenter}, {"warmup", TrialPurpose::Warmup}},
            TrialPurpose::Scored, true);
        tr.spec.task = enum_from<Task>(sp, "task",
                                       {{"flick", Task::Flick}, {"micro", Task::Micro}, {"switch", Task::Switch}, {"track", Task::Track}},
                                       Task::Flick, false);
        tr.spec.to_anchor = sp.flag("to_anchor", false);
        tr.spec.nominal_dist_deg = sp.num("nominal_dist_deg", 0.0, 0.0, 180.0);
        tr.timeout_ms = t.num("timeout_ms", 3000.0, 100.0, 60000.0);
        auto vec2 = [&](const char* key, double& a, double& b) {
            const json* v = t.array_raw(key, true);
            if (!v) return;
            if (v->size() != 2 || !(*v)[0].is_number() || !(*v)[1].is_number() || !std::isfinite((*v)[0].get<double>()) ||
                !std::isfinite((*v)[1].get<double>())) {
                t.error(key, "expected two numbers");
                return;
            }
            a = (*v)[0].get<double>();
            b = (*v)[1].get<double>();
        };
        vec2("target", tr.target_counts.x, tr.target_counts.y);
        vec2("start", tr.start_yaw_deg, tr.start_pitch_deg);
        tr.t_appear_us = t.i64("t_appear_us", 0, true);
        const int64_t dur = t.i64("duration_us", 0, true);
        if (dur < 0 || dur > int64_t(3600) * 1000000) t.error("duration_us", "out of range");
        tr.t_end_us = tr.t_appear_us + dur;
        tr.outcome = enum_from<TrialOutcome>(
            t, "outcome",
            {{"hit", TrialOutcome::Hit}, {"timeout", TrialOutcome::Timeout}, {"interrupted", TrialOutcome::Interrupted},
             {"miss", TrialOutcome::Miss}},
            TrialOutcome::Interrupted, true);
        if (const json* m = t.array_raw("motion", true)) {
            if (m->size() % 5 != 0) {
                t.error("motion", "expected groups of 5 numbers");
            } else {
                int64_t last = std::numeric_limits<int64_t>::min();
                for (size_t i = 0; i < m->size(); i += 5) {
                    bool ok = true;
                    for (size_t k = 0; k < 5; ++k) ok = ok && (*m)[i + k].is_number();
                    if (!ok || !(*m)[i].is_number_integer() || !(*m)[i + 1].is_number_integer() || !(*m)[i + 2].is_number_integer()) {
                        t.error("motion", "malformed sample at " + std::to_string(i / 5));
                        break;
                    }
                    MotionSample ms;
                    const int64_t rel = (*m)[i].get<int64_t>();
                    if (rel < last) {
                        t.error("motion", "timestamps go backwards at sample " + std::to_string(i / 5));
                        break;
                    }
                    last = rel;
                    ms.t_us = tr.t_appear_us + rel;
                    const int64_t dx = (*m)[i + 1].get<int64_t>(), dy = (*m)[i + 2].get<int64_t>();
                    if (std::llabs(dx) > 1000000 || std::llabs(dy) > 1000000) {
                        t.error("motion", "implausible movement at sample " + std::to_string(i / 5));
                        break;
                    }
                    ms.in_dx = int32_t(dx);
                    ms.in_dy = int32_t(dy);
                    ms.out_dx = (*m)[i + 3].get<double>();
                    ms.out_dy = (*m)[i + 4].get<double>();
                    if (!std::isfinite(ms.out_dx) || !std::isfinite(ms.out_dy)) {
                        t.error("motion", "non-finite output at sample " + std::to_string(i / 5));
                        break;
                    }
                    tr.motion.push_back(ms);
                }
            }
        }
        if (const json* k = t.array_raw("clicks", false))
            for (size_t i = 0; i < k->size(); ++i) {
                const json& a = (*k)[i];
                if (!a.is_array() || a.size() != 3 || !a[0].is_number_integer() || !a[1].is_number() || !a[2].is_number()) {
                    t.error("clicks", "malformed click " + std::to_string(i));
                    break;
                }
                ClickEvent ce;
                ce.t_us = tr.t_appear_us + a[0].get<int64_t>();
                ce.hit = a[1].get<int>() != 0;
                ce.offset_deg = a[2].get<double>();
                tr.clicks.push_back(ce);
            }
        s.trials.push_back(std::move(tr));
    }
    for (const auto& rr : r.arr("rounds", false)) {
        RoundRecord x;
        x.index = rr.i32("index", 0, 0, 1000);
        x.task = enum_from<Task>(rr, "task", {{"flick", Task::Flick}, {"micro", Task::Micro}, {"switch", Task::Switch}, {"track", Task::Track}},
                                 Task::Flick, true);
        x.condition = enum_from<Condition>(rr, "condition", {{"baseline", Condition::Baseline}, {"candidate", Condition::Candidate}},
                                           Condition::Baseline, false);
        x.t_start_us = rr.i64("t_start_us", 0);
        x.t_end_us = rr.i64("t_end_us", 0);
        x.hits = rr.i32("hits", 0, 0, 1000000);
        x.misses = rr.i32("misses", 0, 0, 1000000);
        x.timeouts = rr.i32("timeouts", 0, 0, 1000000);
        x.complete = rr.flag("complete", false);
        s.rounds.push_back(x);
    }
    if (const json* tk = r.array_raw("tracking", false)) {
        const int64_t t0 = r.i64("tracking_t0_us", 0);
        if (tk->size() % 9 != 0) {
            r.error("tracking", "expected groups of 9 numbers");
        } else {
            for (size_t i = 0; i < tk->size(); i += 9) {
                bool ok = (*tk)[i].is_number_integer() && (*tk)[i + 1].is_number_integer();
                for (size_t k = 2; k < 9; ++k) ok = ok && (*tk)[i + k].is_number() && std::isfinite((*tk)[i + k].get<double>());
                if (!ok) {
                    r.error("tracking", "malformed sample at " + std::to_string(i / 9));
                    break;
                }
                TrackSample k;
                k.t_us = t0 + (*tk)[i].get<int64_t>();
                k.round = (*tk)[i + 1].get<int>();
                float* f[7] = {&k.target_yaw, &k.target_pitch, &k.aim_yaw, &k.aim_pitch, &k.target_vyaw, &k.target_vpitch, &k.radius};
                for (size_t q = 0; q < 7; ++q) *f[q] = float((*tk)[i + 2 + q].get<double>());
                s.tracking.push_back(k);
            }
        }
    }
    if (errors.size() != before) return std::nullopt;
    return s;
}

}  // namespace fit
