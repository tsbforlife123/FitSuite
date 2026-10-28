#include "fit/profile.hpp"

#include <cmath>
#include <vector>
#include <cstdio>

#include "fit/timeutil.hpp"

namespace fit {

namespace {
std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}
}  // namespace

SetupProfile make_default_setup() {
    SetupProfile p;
    p.id = make_id("setup");
    p.created_utc = utc_now_iso();
    p.updated_utc = p.created_utc;
    return p;
}

Conditions current_conditions(const SetupProfile& p) {
    Conditions c;
    auto put = [&](const char* k, const std::string& v) {
        if (!v.empty()) c[k] = v;
    };
    put("mouse.model", p.mouse.model);
    put("mouse.device", !p.mouse.vid_pid.empty() ? p.mouse.vid_pid : p.mouse.name);
    if (p.mouse.dpi > 0) c["mouse.dpi"] = std::to_string(p.mouse.dpi);
    put("game.id", p.game.id);
    if (p.game.sensitivity > 0) c["game.sensitivity"] = num(p.game.sensitivity);
    put("pc.cpu", p.pc.cpu);
    put("pc.os", p.pc.os);
    if (!p.pc.gpus.empty()) {
        put("pc.gpu", p.pc.gpus.front().name);
        put("pc.gpu_driver", p.pc.gpus.front().driver);
    }
    put("display.name", p.display.name);
    if (p.display.width > 0 && p.display.height > 0)
        c["display.resolution"] = std::to_string(p.display.width) + "x" + std::to_string(p.display.height);
    if (p.display.refresh_hz > 0) c["display.refresh_hz"] = num(std::round(p.display.refresh_hz * 10.0) / 10.0);
    put("display.overdrive", p.display.overdrive);
    return c;
}

std::vector<std::string> condition_keys(const std::string& tool) {
    if (tool == "mousefit") return {"mouse.model", "mouse.device", "mouse.dpi", "game.id", "game.sensitivity"};
    if (tool == "framefit") return {"pc.cpu", "pc.gpu", "pc.gpu_driver", "pc.os", "display.resolution", "game.id"};
    if (tool == "motionfit") return {"display.name", "display.resolution"};
    return {};
}

Conditions conditions_for(const std::string& tool, const SetupProfile& p) {
    const Conditions all = current_conditions(p);
    Conditions c;
    for (const auto& k : condition_keys(tool)) {
        auto it = all.find(k);
        if (it != all.end()) c[k] = it->second;
    }
    return c;
}

std::string describe_condition_key(const std::string& key) {
    static const std::map<std::string, std::string> names = {
        {"mouse.model", "Mouse model"},    {"mouse.device", "Mouse"},         {"mouse.dpi", "DPI"},
        {"game.id", "Game"},               {"game.sensitivity", "In-game sensitivity"},
        {"pc.cpu", "CPU"},                 {"pc.os", "Windows version"},
        {"pc.gpu", "GPU"},                 {"pc.gpu_driver", "GPU driver"},
        {"display.name", "Monitor"},       {"display.resolution", "Resolution"},
        {"display.refresh_hz", "Refresh rate"}, {"display.overdrive", "Overdrive setting"},
    };
    auto it = names.find(key);
    return it != names.end() ? it->second : key;
}

std::string describe_conditions(const Conditions& c) {
    auto get = [&](const char* k) -> std::string {
        auto it = c.find(k);
        return it == c.end() ? std::string() : it->second;
    };
    std::vector<std::string> parts;
    const std::string game = get("game.id");
    if (!game.empty()) parts.push_back(game == "valorant" ? "VALORANT" : game);
    if (!get("game.sensitivity").empty()) parts.push_back("sensitivity " + get("game.sensitivity"));
    if (!get("mouse.model").empty()) parts.push_back(get("mouse.model"));
    else if (!get("mouse.device").empty()) parts.push_back(get("mouse.device"));
    if (!get("mouse.dpi").empty()) parts.push_back(get("mouse.dpi") + " DPI");
    if (!get("display.name").empty()) parts.push_back(get("display.name"));
    if (!get("display.resolution").empty()) parts.push_back(get("display.resolution"));
    if (!get("display.refresh_hz").empty()) parts.push_back(get("display.refresh_hz") + " Hz");
    if (!get("display.overdrive").empty()) parts.push_back("overdrive " + get("display.overdrive"));
    if (!get("pc.gpu").empty()) parts.push_back(get("pc.gpu"));
    if (!get("pc.gpu_driver").empty()) parts.push_back("driver " + get("pc.gpu_driver"));
    if (!get("pc.cpu").empty()) parts.push_back(get("pc.cpu"));
    if (!get("pc.os").empty()) parts.push_back(get("pc.os"));
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : " · ") + p;
    return out;
}

Staleness check_staleness(const Conditions& recorded, const Conditions& current) {
    Staleness s;
    for (const auto& [k, v] : recorded) {
        auto it = current.find(k);
        if (it == current.end()) continue;  // unknown now: cannot tell
        if (it->second != v) {
            s.outdated = true;
            s.reasons.push_back(describe_condition_key(k) + " changed from " + v + " to " + it->second);
        }
    }
    return s;
}

std::map<std::string, std::string> snapshot_settings(const SetupProfile& p) {
    std::map<std::string, std::string> s;
    s["mouse.dpi"] = std::to_string(p.mouse.dpi);
    s["game.sensitivity"] = num(p.game.sensitivity);
    s["game.scoped_multiplier"] = num(p.game.scoped_multiplier);
    s["game.fps_cap"] = std::to_string(p.game.fps_cap);
    for (const auto& [k, v] : p.game.graphics) s["game.graphics." + k] = v;
    if (p.display.refresh_hz > 0) s["display.refresh_hz"] = num(p.display.refresh_hz);
    if (p.display.width > 0) s["display.resolution"] = std::to_string(p.display.width) + "x" + std::to_string(p.display.height);
    if (!p.display.overdrive.empty()) s["display.overdrive"] = p.display.overdrive;
    s["mousefit.active_profile"] = p.active_mouse_profile.empty() ? "none" : p.active_mouse_profile;
    return s;
}

}  // namespace fit
