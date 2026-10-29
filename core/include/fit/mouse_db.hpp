// The gaming mice the big makers list on their own websites, searchable by name. Some have the specs
// their makers publish (checked on each product page; the page and the date are kept with every
// entry); the rest are names from the maker's product list with every spec left blank until checked.
// Specs catch setup mistakes, such as a DPI the mouse cannot do. Mice do not tell Windows their DPI or software settings, so FitSuite
// never reads them from the mouse; anything not on a maker's page is left blank rather than guessed.
#pragma once

#include <string>
#include <vector>

namespace fit {

struct KnownMouse {
    std::string maker;             // "Logitech"
    std::string model;             // "G Pro X Superlight 2"
    std::string sensor;            // "" when the maker's page does not name it
    int min_dpi = 0, max_dpi = 0;  // 0 = not published
    std::vector<int> dpi_steps;    // only these DPI values (some mice have no free DPI setting)
    int max_polling_hz = 0;        // 0 = not published
    std::string polling_note;      // e.g. "8000 Hz with the HyperPolling dongle, sold separately"
    std::string source;            // where the specs were checked
    std::string checked;           // when ("2026-10-29")

    std::string full_name() const { return maker + " " + model; }
    bool has_specs() const { return !sensor.empty() || max_dpi > 0 || !dpi_steps.empty() || max_polling_hz > 0; }
};

const std::vector<KnownMouse>& known_mice();

// Mice whose "maker model" contains every word of the query (case-insensitive), at most `max`.
std::vector<const KnownMouse*> search_mice(const std::string& query, size_t max = 8);
// The mouse whose full name matches exactly (case-insensitive, extra spaces ignored), or nullptr.
const KnownMouse* find_mouse(const std::string& name);

// What FitSuite can tell from the specs: a DPI the mouse cannot do (warnings) and how the measured
// report rate compares with what it can do (notes, for information only).
struct MouseAdvice {
    std::vector<std::string> warnings;
    std::vector<std::string> notes;
};
MouseAdvice mouse_advice(const KnownMouse& m, int dpi, double measured_polling_hz);

}  // namespace fit
