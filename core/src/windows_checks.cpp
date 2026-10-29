#include "fit/windows_checks.hpp"

namespace fit {

std::vector<WindowsCheck> windows_checks(const WindowsGameSettings& s) {
    using S = WindowsCheck::State;
    std::vector<WindowsCheck> out;
    auto on_off = [](bool b) { return std::string(b ? "On" : "Off"); };

    {
        WindowsCheck c;
        c.name = "Game Mode";
        c.usual = "On (the Windows default)";
        c.settings_uri = "ms-settings:gaming-gamemode";
        c.note = "Windows gives the game in front priority over background work. Leave it on unless a game stutters with it.";
        if (s.game_mode) {
            c.current = on_off(*s.game_mode);
            c.state = *s.game_mode ? S::Fine : S::Look;
        }
        out.push_back(c);
    }
    {
        WindowsCheck c;
        c.name = "Hardware-accelerated GPU scheduling";
        c.usual = "Either (On is the default on newer graphics cards)";
        c.settings_uri = "ms-settings:display-advancedgraphics";
        c.note = "Lets the graphics card schedule its own work. The difference is small either way; it is needed for "
                 "frame generation (DLSS 3). If a game stutters, try the other setting and restart.";
        if (s.hags) {
            c.current = on_off(*s.hags);
            c.state = S::Fine;
        } else {
            c.current = "Not set (Windows decides)";
            c.state = S::Fine;
        }
        out.push_back(c);
    }
    {
        WindowsCheck c;
        c.name = "Power plan";
        c.usual = "Balanced or High performance";
        c.settings_uri = "ms-settings:powersleep";
        c.note = "Power saver slows the processor down, which can cost frames. Balanced is fine on most desktops; on a "
                 "laptop also plug it in.";
        if (s.power_plan == "balanced") c.current = "Balanced", c.state = S::Fine;
        else if (s.power_plan == "high_performance") c.current = "High performance", c.state = S::Fine;
        else if (s.power_plan == "ultimate") c.current = "Ultimate performance", c.state = S::Fine;
        else if (s.power_plan == "power_saver") c.current = "Power saver", c.state = S::Look;
        else if (s.power_plan == "other") c.current = "A custom plan", c.state = S::Fine;
        out.push_back(c);
    }
    {
        WindowsCheck c;
        c.name = "Enhance pointer precision";
        c.usual = "Off";
        c.settings_uri = "ms-settings:mousetouchpad";
        c.note = "Windows' own mouse acceleration for the pointer. Games that read the mouse directly (VALORANT, "
                 "Counter-Strike 2, Apex, Overwatch 2) ignore it, so it only changes the desktop and menus; off keeps "
                 "the pointer the same at every hand speed. It is under Additional mouse settings → Pointer Options.";
        if (s.pointer_precision) {
            c.current = on_off(*s.pointer_precision);
            c.state = *s.pointer_precision ? S::Look : S::Fine;
        }
        out.push_back(c);
    }
    for (auto& c : out)
        if (c.current.empty()) c.current = "Could not be read";
    return out;
}

}  // namespace fit
