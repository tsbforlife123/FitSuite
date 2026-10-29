#include "fit/mouse_db.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace fit {

const std::vector<KnownMouse>& known_mice() {
    // Each entry was checked on the maker's product page on the date given. Leave a field blank (0 or
    // "") when the page does not state it; never fill it in from memory.
    static const std::vector<KnownMouse> mice = {
        {"Logitech", "G Pro X Superlight 2", "HERO 2", 100, 44000, {}, 8000,
         "The spec table says 32,000 DPI; the page says up to 44,000 with current firmware.",
         "logitechg.com/en-us/products/gaming-mice/pro-x2-superlight-wireless-mouse.html", "2026-10-29"},
        {"Logitech", "G Pro X Superlight", "HERO", 100, 25600, {}, 1000, "",
         "logitechg.com/en-us/products/gaming-mice/pro-x-superlight-wireless-mouse.html", "2026-10-29"},
        {"Logitech", "G305 Lightspeed", "HERO", 200, 12000, {}, 1000, "",
         "logitechg.com/en-us/products/gaming-mice/g305-lightspeed-wireless-gaming-mouse.html", "2026-10-29"},
        {"Logitech", "G502 HERO", "HERO", 100, 25600, {}, 1000, "",
         "logitechg.com/en-us/products/gaming-mice/g502-hero-gaming-mouse.html", "2026-10-29"},
        {"Razer", "Viper V3 Pro", "Focus Pro 35K Gen-2", 0, 35000, {}, 8000, "8000 Hz with Razer's HyperPolling technology.",
         "razer.com/gaming-mice/razer-viper-v3-pro", "2026-10-29"},
        {"Razer", "DeathAdder V3 Pro", "Focus Pro 30K", 0, 30000, {}, 8000,
         "8000 Hz with the HyperPolling Wireless Dongle, sold separately.", "razer.com/gaming-mice/razer-deathadder-v3-pro",
         "2026-10-29"},
        {"Razer", "Viper V2 Pro", "Focus Pro 30K", 0, 30000, {}, 8000,
         "8000 Hz with the HyperPolling Wireless Dongle, sold separately.", "razer.com/gaming-mice/razer-viper-v2-pro",
         "2026-10-29"},
        {"Razer", "Basilisk V3", "Focus+ 26K", 0, 26000, {}, 0, "", "razer.com/gaming-mice/razer-basilisk-v3", "2026-10-29"},
        {"Zowie", "EC2-DW", "", 0, 0, {400, 800, 1000, 1200, 1600, 3200}, 4000, "",
         "zowie.benq.com/en-us/mouse/ec2-dw.html", "2026-10-29"},
        {"SteelSeries", "Aerox 3 Wireless", "", 0, 18000, {}, 0, "", "steelseries.com/gaming-mice/aerox-3-2022", "2026-10-29"},
    };
    return mice;
}

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::vector<std::string> words(const std::string& s) {
    std::istringstream is(lower(s));
    std::vector<std::string> w;
    for (std::string x; is >> x;) w.push_back(x);
    return w;
}

std::string squash(const std::string& s) {
    std::string out;
    for (const auto& w : words(s)) out += (out.empty() ? "" : " ") + w;
    return out;
}

std::string with_commas(int v) {
    std::string s = std::to_string(v);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
    return s;
}

}  // namespace

std::vector<const KnownMouse*> search_mice(const std::string& query, size_t max) {
    std::vector<const KnownMouse*> out;
    const auto q = words(query);
    if (q.empty()) return out;
    for (const auto& m : known_mice()) {
        const std::string name = lower(m.full_name());
        bool all = true;
        for (const auto& w : q) all = all && name.find(w) != std::string::npos;
        if (all) out.push_back(&m);
        if (out.size() >= max) break;
    }
    return out;
}

const KnownMouse* find_mouse(const std::string& name) {
    const std::string n = squash(name);
    if (n.empty()) return nullptr;
    for (const auto& m : known_mice())
        if (squash(m.full_name()) == n) return &m;
    return nullptr;
}

MouseAdvice mouse_advice(const KnownMouse& m, int dpi, double measured_hz) {
    MouseAdvice a;
    if (dpi > 0) {
        if (!m.dpi_steps.empty()) {
            if (std::find(m.dpi_steps.begin(), m.dpi_steps.end(), dpi) == m.dpi_steps.end()) {
                std::string steps;
                for (int s : m.dpi_steps) steps += (steps.empty() ? "" : ", ") + std::to_string(s);
                a.warnings.push_back("The " + m.model + " offers only " + steps + " DPI, so " + std::to_string(dpi) +
                                     " is probably not your setting. Check the DPI on your mouse.");
            }
        } else if (m.max_dpi > 0 && dpi > m.max_dpi) {
            a.warnings.push_back(std::to_string(dpi) + " DPI is more than the " + m.model + " can do (up to " +
                                 with_commas(m.max_dpi) + "). Check the DPI in your mouse software.");
        } else if (m.min_dpi > 0 && dpi < m.min_dpi) {
            a.warnings.push_back(std::to_string(dpi) + " DPI is less than the " + m.model + " can do (from " +
                                 std::to_string(m.min_dpi) + "). Check the DPI in your mouse software.");
        }
    }
    if (measured_hz > 0.0 && m.max_polling_hz > 0) {
        char buf[240];
        if (measured_hz > m.max_polling_hz * 1.3) {
            std::snprintf(buf, sizeof buf,
                          "FitSuite measured about %.0f reports a second, more than the %s's %d Hz: is this the mouse you "
                          "measured with?",
                          measured_hz, m.model.c_str(), m.max_polling_hz);
            a.warnings.push_back(buf);
        } else if (measured_hz < m.max_polling_hz * 0.6) {
            std::snprintf(buf, sizeof buf,
                          "Your mouse sends about %.0f reports a second; the %s can do up to %d Hz in its software. "
                          "Either works for the fix; measure again if you change it.",
                          measured_hz, m.model.c_str(), m.max_polling_hz);
            a.notes.push_back(buf);
        }
    }
    return a;
}

}  // namespace fit
