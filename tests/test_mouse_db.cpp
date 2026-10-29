#include <set>

#include "doctest.h"
#include "fit/mouse_db.hpp"

using namespace fit;

TEST_CASE("the mouse list: every entry is named, sourced and dated, with no made-up fields") {
    REQUIRE(known_mice().size() >= 200);
    int with_specs = 0;
    std::set<std::string> names;
    for (const auto& m : known_mice()) {
        CHECK_FALSE(m.maker.empty());
        CHECK_FALSE(m.model.empty());
        CHECK_FALSE(m.source.empty());
        CHECK(m.checked.size() == 10);  // YYYY-MM-DD
        CHECK(names.insert(m.full_name()).second);  // no duplicates
        if (m.min_dpi > 0) CHECK(m.max_dpi > m.min_dpi);
        CHECK(m.max_dpi <= 100000);
        CHECK(m.max_polling_hz <= 8000);
        if (m.has_specs()) ++with_specs;
        else CHECK(m.polling_note.empty());  // a name-only entry says nothing it did not check
    }
    CHECK(with_specs >= 30);
}

TEST_CASE("searching the mouse list as you type") {
    auto r = search_mice("superlight");
    REQUIRE(r.size() == 6);  // Superlight, Superlight 2, 2c, 2 DEX, 2 SE, G305 X Superlight
    r = search_mice("SUPERLIGHT 2");
    REQUIRE_FALSE(r.empty());
    CHECK(r.front()->model == "G Pro X Superlight 2");
    CHECK(search_mice("razer viper").size() == 6);
    CHECK(search_mice("viper", 3).size() == 3);  // at most as many as asked for
    const KnownMouse* names_only = find_mouse("Pulsar X2 v3");
    REQUIRE(names_only);
    CHECK_FALSE(names_only->has_specs());
    CHECK(mouse_advice(*names_only, 800, 1000).warnings.empty());  // no specs, no warnings
    CHECK(search_mice("").empty());
    CHECK(search_mice("no such mouse").empty());

    const KnownMouse* m = find_mouse("  logitech   g305 lightspeed ");
    REQUIRE(m);
    CHECK(m->max_dpi == 12000);
    CHECK_FALSE(find_mouse("Logitech G305"));  // only exact names count as chosen
}

TEST_CASE("mouse advice: a DPI the mouse cannot do, and the report rate") {
    const KnownMouse* g305 = find_mouse("Logitech G305 Lightspeed");
    REQUIRE(g305);
    CHECK(mouse_advice(*g305, 800, 1000).warnings.empty());
    CHECK(mouse_advice(*g305, 800, 1000).notes.empty());
    const auto high = mouse_advice(*g305, 16000, 1000);
    REQUIRE(high.warnings.size() == 1);
    CHECK(high.warnings[0].find("12,000") != std::string::npos);
    CHECK_FALSE(mouse_advice(*g305, 100, 0).warnings.empty());  // below its 200 DPI minimum
    // More reports than the mouse can send: probably a different mouse.
    CHECK_FALSE(mouse_advice(*g305, 800, 4000).warnings.empty());

    const KnownMouse* sl2 = find_mouse("Logitech G Pro X Superlight 2");
    REQUIRE(sl2);
    const auto slow = mouse_advice(*sl2, 800, 1000);  // runs at 1000 Hz, can do 8000: a note, not a warning
    CHECK(slow.warnings.empty());
    CHECK(slow.notes.size() == 1);

    // Mice with fixed DPI steps.
    const KnownMouse* zowie = find_mouse("Zowie EC2-DW");
    REQUIRE(zowie);
    CHECK(mouse_advice(*zowie, 800, 0).warnings.empty());
    CHECK_FALSE(mouse_advice(*zowie, 850, 0).warnings.empty());
    // Unknown polling rate: no polling advice.
    const KnownMouse* aerox = find_mouse("SteelSeries Aerox 3 Wireless");
    REQUIRE(aerox);
    CHECK(mouse_advice(*aerox, 800, 125).notes.empty());
}
