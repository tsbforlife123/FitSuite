#include "doctest.h"
#include "fit/game_activation.hpp"

using namespace fit;

TEST_CASE("executable names are normalised") {
    CHECK(normalize_exe("C:\\Riot Games\\VALORANT\\live\\ShooterGame\\Binaries\\Win64\\VALORANT-Win64-Shipping.exe") ==
          "valorant-win64-shipping.exe");
    CHECK(normalize_exe(" CS2.EXE ") == "cs2.exe");
    CHECK(game_display_name("cs2.exe") == "Counter-Strike 2");
    CHECK(game_display_name("SomeStoryGame.exe") == "somestorygame");
    for (const auto& g : known_shooters()) CHECK(normalize_exe(g.exe) == g.exe);  // the list is stored normalised
}

TEST_CASE("the fix follows the game in front, after it settles") {
    ActivationSwitcher s(1.0);
    const std::vector<std::string> games = {"valorant-win64-shipping.exe", "cs2.exe"};

    // Start-up: state unknown, FitSuite in front -> off at once.
    auto d = s.update("fitsuite.exe", games, 0.0, std::nullopt);
    REQUIRE(d);
    CHECK_FALSE(*d);
    std::optional<bool> on = false;

    // VALORANT comes to the front: nothing for the first second, then on.
    CHECK_FALSE(s.update("VALORANT-Win64-Shipping.exe", games, 10.0, on));
    CHECK_FALSE(s.update("VALORANT-Win64-Shipping.exe", games, 10.5, on));
    d = s.update("VALORANT-Win64-Shipping.exe", games, 11.0, on);
    REQUIRE(d);
    CHECK(*d);
    on = true;

    // A quick Alt-Tab to the browser and back does not switch anything.
    CHECK_FALSE(s.update("chrome.exe", games, 20.0, on));
    CHECK_FALSE(s.update("chrome.exe", games, 20.6, on));
    CHECK_FALSE(s.update("valorant-win64-shipping.exe", games, 20.9, on));
    CHECK_FALSE(s.update("valorant-win64-shipping.exe", games, 25.0, on));

    // Unknown foreground keeps the state.
    CHECK_FALSE(s.update("", games, 30.0, on));

    // A story game stays in front: the fix goes off.
    CHECK_FALSE(s.update("StoryGame.exe", games, 40.0, on));
    d = s.update("StoryGame.exe", games, 41.2, on);
    REQUIRE(d);
    CHECK_FALSE(*d);
}

TEST_CASE("an empty game list never turns the fix on") {
    ActivationSwitcher s(0.0);
    auto d = s.update("cs2.exe", {}, 0.0, std::nullopt);
    REQUIRE(d);
    CHECK_FALSE(*d);
    CHECK_FALSE(s.update("cs2.exe", {}, 5.0, false));
}

TEST_CASE("FitSuite in front switches the fix off at once, so its tests measure the player's own settings") {
    ActivationSwitcher s(1.0);
    const std::vector<std::string> games = {"valorant-win64-shipping.exe", "fitsuite.exe"};  // even if listed by mistake
    auto d = s.update("FitSuite.exe", games, 50.0, true);
    REQUIRE(d);
    CHECK_FALSE(*d);
    // Already off: nothing to do.
    CHECK_FALSE(s.update("fitsuite.exe", games, 50.3, false));
    // Back to the game: waits for the settle time as usual.
    CHECK_FALSE(s.update("valorant-win64-shipping.exe", games, 51.0, false));
    d = s.update("valorant-win64-shipping.exe", games, 52.0, false);
    REQUIRE(d);
    CHECK(*d);
}

TEST_CASE("games with several executables share one display name") {
    CHECK(game_display_name("r5apex_dx12.exe") == "Apex Legends");
    CHECK(game_display_name("r5apex.exe") == "Apex Legends");
    CHECK(game_display_name("RainbowSix_Vulkan.exe") == "Rainbow Six Siege");
}

TEST_CASE("turn distances convert between games with a known yaw") {
    // VALORANT 0.4 at 800 DPI: 360 / (0.07 × 0.4 × 800) counts = 16.07 inches = 40.8 cm.
    CHECK(cm_per_360(game_yaw("VALORANT-Win64-Shipping.exe"), 0.4, 800) == doctest::Approx(40.82).epsilon(0.001));
    // The usual VALORANT → CS2 factor of about 3.18 gives the same turn.
    CHECK(cm_per_360(game_yaw("cs2.exe"), 0.4 * 0.07 / 0.022, 800) == doctest::Approx(40.82).epsilon(0.001));
    CHECK(game_yaw("r5apex_dx12.exe") == doctest::Approx(0.022));
    CHECK(game_yaw("overwatch.exe") == doctest::Approx(0.0066));
    // FOV-dependent games have no conversion; unknown games neither.
    CHECK(game_yaw("fortniteclient-win64-shipping.exe") == 0.0);
    CHECK(game_yaw("storygame.exe") == 0.0);
    CHECK(cm_per_360(0.0, 1.0, 800) == 0.0);
    CHECK(cm_per_360(0.022, 1.0, 0) == 0.0);
}
