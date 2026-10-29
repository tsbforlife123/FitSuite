#include "fit/rawaccel_export.hpp"

#include <cmath>
#include <cstdio>

#include "fit/optimize.hpp"
#include "nlohmann/json.hpp"

namespace fit {

namespace {
constexpr const char* kRot = "Degrees of rotation";
constexpr const char* kYX = "Y/X output DPI ratio (vertical sens multiplier)";
constexpr const char* kLR = "L/R output DPI ratio (left sens multiplier)";
constexpr const char* kUD = "U/D output DPI ratio (up sens multiplier)";
constexpr const char* kDPI = "Output DPI";
constexpr const char* kSnap = "Degrees of angle snapping";
constexpr const char* kCap = "Input Speed Cap";
constexpr const char* kAccelX = "Whole or horizontal accel parameters";
constexpr const char* kAccelY = "Vertical accel parameters";

std::string fmt(double v, int decimals) {
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", decimals, v);
    return b;
}
}  // namespace

std::vector<RawAccelField> rawaccel_fields(const RawAccelProfile& p) {
    return {
        {"Output DPI", "your current value × " + fmt(p.sens, 4) + "  (1000 → " + fmt(p.sens * 1000.0, 0) + ")"},
        {"Y/X Ratio", fmt(p.yx_ratio, 4)},
        {"Rotation", fmt(p.rotation_deg, 2) + "°"},
        {"L/R Ratio (left multiplier)", fmt(p.lr_ratio, 4)},
        {"U/D Ratio (up multiplier)", fmt(p.ud_ratio, 4)},
        {"Acceleration", "Off (no acceleration)"},
        {"Angle Snapping", "0°"},
    };
}

RawAccelProfile compose_rawaccel(const RawAccelProfile& existing, const RawAccelProfile& correction, double* max_error) {
    std::vector<Vec2> dirs;
    for (int i = 0; i < 360; ++i) dirs.push_back(raw_unit_from_screen_angle((i + 0.5) * kDegToRad));
    std::vector<Vec2> want;
    for (const auto& d : dirs) want.push_back(correction.apply(existing.apply(d)));
    auto build = [](const std::vector<double>& u) {
        RawAccelProfile r;
        r.rotation_deg = u[0];
        r.sens = std::exp(u[1]);
        r.yx_ratio = std::exp(u[2]);
        r.lr_ratio = std::exp(u[3]);
        r.ud_ratio = std::exp(u[4]);
        return r;
    };
    auto err = [&](const std::vector<double>& u) {
        const RawAccelProfile r = build(u);
        double s = 0.0;
        for (size_t i = 0; i < dirs.size(); ++i) {
            const Vec2 d = r.apply(dirs[i]) - want[i];
            s += dot(d, d);
        }
        return s;
    };
    const std::vector<double> x0 = {existing.rotation_deg + correction.rotation_deg, std::log(existing.sens * correction.sens),
                                    std::log(existing.yx_ratio * correction.yx_ratio),
                                    std::log(existing.lr_ratio * correction.lr_ratio),
                                    std::log(existing.ud_ratio * correction.ud_ratio)};
    auto res = nelder_mead(err, x0, 0.02, 6000, 1e-16);
    const RawAccelProfile out = build(res.x);
    if (max_error) {
        double m = 0.0;
        for (size_t i = 0; i < dirs.size(); ++i) m = std::max(m, length(out.apply(dirs[i]) - want[i]) / length(want[i]));
        *max_error = m;
    }
    return out;
}

std::optional<RawAccelPatchResult> patch_rawaccel_settings(std::string_view text, const RawAccelProfile& correction,
                                                           const std::string& profile_name, std::string* error) {
    using nlohmann::json;
    auto fail = [&](const std::string& e) -> std::optional<RawAccelPatchResult> {
        if (error) *error = e;
        return std::nullopt;
    };
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return fail("This is not a JSON file.");
    if (!j.contains("profiles") || !j["profiles"].is_array() || j["profiles"].empty())
        return fail("This does not look like a Raw Accel settings.json (no \"profiles\" list).");
    json* prof = nullptr;
    for (auto& p : j["profiles"]) {
        if (!p.is_object()) continue;
        if (profile_name.empty() || (p.contains("name") && p["name"].is_string() && p["name"].get<std::string>() == profile_name)) {
            prof = &p;
            break;
        }
    }
    if (!prof) return fail("Profile \"" + profile_name + "\" was not found in the Raw Accel settings.");
    for (const char* k : {kRot, kYX, kLR, kUD, kDPI})
        if (!prof->contains(k) || !(*prof)[k].is_number())
            return fail(std::string("The Raw Accel profile has no numeric \"") + k + "\" field (unsupported Raw Accel version?).");

    RawAccelPatchResult r;
    r.existing.rotation_deg = (*prof)[kRot].get<double>();
    r.existing.yx_ratio = (*prof)[kYX].get<double>();
    r.existing.lr_ratio = (*prof)[kLR].get<double>();
    r.existing.ud_ratio = (*prof)[kUD].get<double>();
    const double existing_dpi = (*prof)[kDPI].get<double>();
    r.existing.sens = 1.0;  // the Output DPI is kept and scaled separately
    if (!(existing_dpi > 0)) return fail("The Raw Accel profile has an invalid Output DPI.");

    auto mode_of = [&](const char* key) -> std::string {
        if (!prof->contains(key) || !(*prof)[key].is_object()) return "";
        const json& a = (*prof)[key];
        if (a.contains("mode") && a["mode"].is_string()) return a["mode"].get<std::string>();
        return "";
    };
    const std::string mx = mode_of(kAccelX), my = mode_of(kAccelY);
    if (!mx.empty() && mx != "noaccel")
        r.warnings.push_back("This Raw Accel profile uses acceleration (\"" + mx +
                             "\"). MouseFit's validation assumed none, so the result may not transfer.");
    if (!my.empty() && my != "noaccel" && my != mx)
        r.warnings.push_back("The vertical acceleration settings are in use (\"" + my + "\").");
    if (prof->contains(kSnap) && (*prof)[kSnap].is_number() && (*prof)[kSnap].get<double>() != 0.0)
        r.warnings.push_back("Angle snapping is enabled in Raw Accel; it changes diagonal movement and was not part of the calibration.");
    if (prof->contains(kCap) && (*prof)[kCap].is_number() && (*prof)[kCap].get<double>() > 0.0)
        r.warnings.push_back("An input speed cap is set in Raw Accel; very fast flicks are clamped.");

    RawAccelProfile corr = correction;
    const double corr_sens = corr.sens;
    corr.sens = 1.0;
    r.written = compose_rawaccel(r.existing, corr, &r.composition_error);
    const double sens_from_fit = r.written.sens;  // composition may move overall scale slightly
    r.written.sens = 1.0;
    if (r.composition_error > 0.002)
        r.warnings.push_back("Your existing Raw Accel rotation/ratios and the correction cannot be combined exactly; the closest "
                             "Raw Accel settings differ by up to " + fmt(r.composition_error * 100.0, 1) + "% in some directions.");

    (*prof)[kRot] = std::round(r.written.rotation_deg * 1e4) / 1e4;
    (*prof)[kYX] = std::round(r.written.yx_ratio * 1e6) / 1e6;
    (*prof)[kLR] = std::round(r.written.lr_ratio * 1e6) / 1e6;
    (*prof)[kUD] = std::round(r.written.ud_ratio * 1e6) / 1e6;
    const double new_dpi = existing_dpi * corr_sens * sens_from_fit;
    (*prof)[kDPI] = std::round(new_dpi * 100.0) / 100.0;
    r.written.sens = new_dpi / 1000.0;

    // The speed part: Raw Accel's lookup-table acceleration in sensitivity mode ("Gain / Velocity"
    // off), in the whole/horizontal accel parameters (used for all movement in Raw Accel's default
    // "whole" mode).
    if (!correction.speed.empty()) {
        const bool other_accel = !mx.empty() && mx != "noaccel";
        if (other_accel || !prof->contains(kAccelX) || !(*prof)[kAccelX].is_object()) {
            r.warnings.push_back("The speed part of your fix was left out: this Raw Accel profile already uses its own "
                                 "acceleration, and FitSuite does not combine the two.");
        } else {
            // With a DPI set for the device, Raw Accel measures speed in inches per second instead of
            // counts per millisecond.
            const char* kDevDpi = "DPI (normalizes input speed unit: counts/ms -> in/s)";
            double dev_dpi = 0.0;
            if (j.contains("defaultDeviceConfig") && j["defaultDeviceConfig"].is_object() &&
                j["defaultDeviceConfig"].contains(kDevDpi) && j["defaultDeviceConfig"][kDevDpi].is_number())
                dev_dpi = j["defaultDeviceConfig"][kDevDpi].get<double>();
            if (j.contains("devices") && j["devices"].is_array())
                for (const auto& d : j["devices"])
                    if (d.is_object() && d.contains("config") && d["config"].is_object() && d["config"].contains(kDevDpi) &&
                        d["config"][kDevDpi].is_number() && d["config"][kDevDpi].get<double>() != dev_dpi)
                        r.warnings.push_back("A mouse has its own DPI setting in Raw Accel; the speed part of your fix "
                                             "assumes the default device settings.");
            const double to_unit = dev_dpi > 0.0 ? 1000.0 / dev_dpi : 1.0;
            json data = json::array();
            for (const auto& [x, y] : correction.speed.points) {
                data.push_back(double(float(x * to_unit)));
                data.push_back(double(float(y)));
            }
            json& ax = (*prof)[kAccelX];
            ax["mode"] = "lut";
            ax["Gain / Velocity"] = false;
            ax["data"] = data;
            r.written.speed = correction.speed;
            // The fix was tested with Raw Accel's default speed measurement; say so when it differs.
            const char* kSpeedArgs = "Input speed calculation parameters";
            if (prof->contains(kSpeedArgs) && (*prof)[kSpeedArgs].is_object()) {
                const json& sa = (*prof)[kSpeedArgs];
                bool plain = true;
                for (const auto& [key, val] : sa.items()) {
                    if (val.is_boolean() && key.rfind("Whole", 0) == 0 && !val.get<bool>()) plain = false;
                    if (val.is_number() && key == "lpNorm" && val.get<double>() != 2.0) plain = false;
                    if (val.is_number() && key.rfind("Time in ms", 0) == 0 && val.get<double>() != 0.0) plain = false;
                }
                if (!plain)
                    r.warnings.push_back("Raw Accel's input speed settings (by component, Lp norm or smoothing) differ "
                                         "from the defaults the speed part of your fix was tested with.");
            }
            for (const char* w : {"Stretches domain for horizontal vs vertical inputs", "Stretches accel range for horizontal vs vertical inputs"})
                if (prof->contains(w) && (*prof)[w].is_object() &&
                    ((*prof)[w].value("x", 1.0) != 1.0 || (*prof)[w].value("y", 1.0) != 1.0))
                    r.warnings.push_back("Raw Accel stretches speed differently for horizontal and vertical movement; the "
                                         "speed part of your fix was tested without that.");
        }
    }
    r.json = j.dump(2);
    return r;
}

}  // namespace fit
