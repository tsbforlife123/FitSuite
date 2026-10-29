#include "doctest.h"
#include "fit/windows_checks.hpp"

using namespace fit;
using S = WindowsCheck::State;

TEST_CASE("Windows checks: usual settings are fine, unusual ones are pointed out, unknown ones say so") {
    WindowsGameSettings w;
    w.game_mode = true;
    w.hags = true;
    w.power_plan = "balanced";
    w.pointer_precision = false;
    auto c = windows_checks(w);
    REQUIRE(c.size() == 4);
    for (const auto& x : c) {
        CHECK_MESSAGE(x.state == S::Fine, x.name);
        CHECK_FALSE(x.settings_uri.empty());
        CHECK_FALSE(x.note.empty());
    }

    w.game_mode = false;
    w.power_plan = "power_saver";
    w.pointer_precision = true;
    c = windows_checks(w);
    CHECK(c[0].state == S::Look);
    CHECK(c[1].state == S::Fine);  // GPU scheduling: either is fine
    CHECK(c[2].state == S::Look);
    CHECK(c[2].current == "Power saver");
    CHECK(c[3].state == S::Look);

    c = windows_checks(WindowsGameSettings{});
    CHECK(c[0].state == S::Unknown);
    CHECK(c[0].current == "Could not be read");
    CHECK(c[1].current == "Not set (Windows decides)");
    CHECK(c[2].state == S::Unknown);
    CHECK(c[3].state == S::Unknown);
}
