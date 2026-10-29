// MouseFit "only in my shooters": decides when the Raw Accel fix should be active, from the name
// of the program in front. Nothing here looks at a game's screen, memory or input.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace fit {

struct KnownGame {
    const char* exe;   // lower-case executable name
    const char* name;  // display name
    // Degrees of turn per mouse count at sensitivity 1 (hip-fire), for games where it is fixed and
    // published; 0 = no simple conversion (FOV-dependent or not verified), enter cm/360 instead.
    double yaw = 0.0;
    // Where the sensitivity is in the game's menus, when FitSuite knows it; nullptr otherwise.
    const char* sens_where = nullptr;
};

// The game the player measures with (SetupProfile::game.id). "valorant" (the id used before other
// games could be chosen) and "" mean VALORANT; any other id is the game's executable name.
std::string main_game_exe(const std::string& game_id);
std::string game_id_for_exe(const std::string& exe);
// Where to find the sensitivity in a game's menus: a known path, or general advice.
std::string game_sens_where(const std::string& exe);

// Shooters FitSuite recognises by executable name. Some games have several executables (DirectX 12
// or Vulkan builds) under the same display name. Players can add any other game.
const std::vector<KnownGame>& known_shooters();

// Degrees per count at sensitivity 1 for a known shooter, 0 when FitSuite has no conversion.
double game_yaw(const std::string& exe);
// Mouse distance for a full 360° turn, in cm; 0 when any input is missing.
double cm_per_360(double yaw_deg_per_count, double sensitivity, int dpi);

// "C:\\Games\\VALORANT-Win64-Shipping.exe" -> "valorant-win64-shipping.exe"
std::string normalize_exe(const std::string& path_or_name);
// Known display name, or the executable name without ".exe".
std::string game_display_name(const std::string& exe);

// Turns the fix on while a listed game is in front and off otherwise. A change is only made after
// the new state has held for `settle_s` seconds, so a quick Alt-Tab does not flip settings back and
// forth (Raw Accel itself takes about a second to accept new settings). Two cases are decided at
// once: the first decision after start-up, when the current state is unknown, and switching off
// because FitSuite itself (`self`) came to the front, so its own tests always measure the player's
// settings without the fix.
class ActivationSwitcher {
public:
    explicit ActivationSwitcher(double settle_s = 1.0) : settle_s_(settle_s) {}
    // foreground: executable in front ("" = unknown, keeps the current state).
    // current: what Raw Accel is believed to use now (nullopt = unknown).
    // Returns the state to switch to, or nullopt for "leave it".
    std::optional<bool> update(const std::string& foreground, const std::vector<std::string>& games, double now_s,
                               std::optional<bool> current, const std::string& self = "fitsuite.exe");
    void reset() { pending_.reset(); }
    // When a pending change will be made if the program in front stays the same (nullopt: none).
    std::optional<double> deadline() const {
        if (!pending_) return std::nullopt;
        return pending_since_ + settle_s_;
    }

private:
    double settle_s_;
    std::optional<bool> pending_;
    double pending_since_ = 0.0;
};

}  // namespace fit
