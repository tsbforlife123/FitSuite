// Read-only checks of a few Windows settings that matter for games (Settings → Windows settings for
// games). FitSuite only reads them and says what they are set to; changing them is left to the player,
// through the Windows settings page each row opens.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace fit {

// What the platform layer read. An empty optional or string means "could not be read".
struct WindowsGameSettings {
    std::optional<bool> game_mode;          // Settings → Gaming → Game Mode
    std::optional<bool> hags;               // Hardware-accelerated GPU scheduling (empty: Windows decides)
    std::string power_plan;                 // "balanced" | "high_performance" | "ultimate" | "power_saver" | "other"
    std::optional<bool> pointer_precision;  // "Enhance pointer precision" (Windows pointer acceleration)
};

struct WindowsCheck {
    enum class State { Fine, Look, Unknown };
    std::string name;
    std::string current;       // "On", "Off", "Balanced", …
    std::string usual;         // the usual choice for games
    std::string note;          // one or two plain sentences
    std::string settings_uri;  // the Windows settings page that changes it
    State state = State::Unknown;
};

std::vector<WindowsCheck> windows_checks(const WindowsGameSettings& s);

}  // namespace fit
