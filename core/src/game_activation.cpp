#include "fit/game_activation.hpp"

#include <algorithm>
#include <cctype>

namespace fit {

const std::vector<KnownGame>& known_shooters() {
    // Yaw values: VALORANT 0.07, Source-engine games 0.022, Overwatch 2 and Call of Duty 0.0066 degrees
    // per count at sensitivity 1. The others have FOV-dependent or unpublished scales, so FitSuite
    // asks for the turn distance (cm/360) instead of converting.
    static const std::vector<KnownGame> games = {
        {"valorant-win64-shipping.exe", "VALORANT", 0.07},
        {"cs2.exe", "Counter-Strike 2", 0.022},
        {"r5apex.exe", "Apex Legends", 0.022},
        {"r5apex_dx12.exe", "Apex Legends", 0.022},
        {"overwatch.exe", "Overwatch 2", 0.0066},
        {"fortniteclient-win64-shipping.exe", "Fortnite", 0.0},
        {"rainbowsix.exe", "Rainbow Six Siege", 0.0},
        {"rainbowsix_vulkan.exe", "Rainbow Six Siege", 0.0},
        {"cod.exe", "Call of Duty", 0.0066},
        {"marvel-win64-shipping.exe", "Marvel Rivals", 0.0},
        {"discovery.exe", "THE FINALS", 0.0},
        {"tslgame.exe", "PUBG: Battlegrounds", 0.0},
        {"escapefromtarkov.exe", "Escape from Tarkov", 0.0},
        {"destiny2.exe", "Destiny 2", 0.0},
        {"haloinfinite.exe", "Halo Infinite", 0.0},
        {"rustclient.exe", "Rust", 0.0},
    };
    return games;
}

double game_yaw(const std::string& exe) {
    const std::string e = normalize_exe(exe);
    for (const auto& g : known_shooters())
        if (e == g.exe) return g.yaw;
    return 0.0;
}

double cm_per_360(double yaw, double sensitivity, int dpi) {
    if (!(yaw > 0.0) || !(sensitivity > 0.0) || dpi <= 0) return 0.0;
    return 360.0 / (yaw * sensitivity * double(dpi)) * 2.54;
}

std::string normalize_exe(const std::string& path_or_name) {
    std::string s = path_or_name;
    const size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    s = s.substr(a);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string game_display_name(const std::string& exe) {
    const std::string e = normalize_exe(exe);
    for (const auto& g : known_shooters())
        if (e == g.exe) return g.name;
    if (e.size() > 4 && e.compare(e.size() - 4, 4, ".exe") == 0) return e.substr(0, e.size() - 4);
    return e;
}

std::optional<bool> ActivationSwitcher::update(const std::string& foreground, const std::vector<std::string>& games,
                                               double now_s, std::optional<bool> current, const std::string& self) {
    if (foreground.empty()) return std::nullopt;  // unknown: keep whatever is active
    const std::string fg = normalize_exe(foreground);
    const bool is_self = !self.empty() && fg == normalize_exe(self);
    bool want = false;
    for (const auto& g : games)
        if (!is_self && normalize_exe(g) == fg) want = true;
    if (!current || (is_self && *current)) {  // first decision, or FitSuite in front: no reason to wait
        pending_.reset();
        return want;
    }
    if (want == *current) {
        pending_.reset();
        return std::nullopt;
    }
    if (!pending_ || *pending_ != want) {
        pending_ = want;
        pending_since_ = now_s;
    }
    if (now_s - pending_since_ >= settle_s_) {
        pending_.reset();
        return want;
    }
    return std::nullopt;
}

}  // namespace fit
