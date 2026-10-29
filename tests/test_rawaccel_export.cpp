#include <cmath>

#include "doctest.h"
#include "fit/rawaccel_export.hpp"
#include "nlohmann/json.hpp"

using namespace fit;
using nlohmann::json;

namespace {

json minimal_settings(double rotation = 0, double yx = 1, double dpi = 1000, const char* mode = "noaccel") {
    json accel = {{"mode", mode}, {"Gain / Velocity", true}};
    json prof = {{"name", "default"},
                 {"Whole or horizontal accel parameters", accel},
                 {"Vertical accel parameters", accel},
                 {"Output DPI", dpi},
                 {"Y/X output DPI ratio (vertical sens multiplier)", yx},
                 {"L/R output DPI ratio (left sens multiplier)", 1.0},
                 {"U/D output DPI ratio (up sens multiplier)", 1.0},
                 {"Degrees of rotation", rotation},
                 {"Degrees of angle snapping", 0.0},
                 {"Input Speed Cap", 0.0}};
    return {{"### Accel modes ###", "classic | jump | natural | synchronous | power | lut | noaccel"},
            {"version", "1.7.0"},
            {"defaultDeviceConfig", {{"disable", false}, {"DPI (normalizes input speed unit: counts/ms -> in/s)", 0}}},
            {"profiles", json::array({prof})},
            {"devices", json::array()}};
}

}  // namespace

TEST_CASE("patching default Raw Accel settings writes the correction and keeps other fields") {
    RawAccelProfile c;
    c.yx_ratio = 1.06;
    c.rotation_deg = -1.5;
    c.sens = 0.97;
    std::string err;
    auto r = patch_rawaccel_settings(minimal_settings().dump(), c, "", &err);
    REQUIRE_MESSAGE(r, err);
    CHECK(r->warnings.empty());
    CHECK(r->composition_error < 1e-6);
    json out = json::parse(r->json);
    const json& p = out["profiles"][0];
    CHECK(p["Y/X output DPI ratio (vertical sens multiplier)"].get<double>() == doctest::Approx(1.06).epsilon(1e-4));
    CHECK(p["Degrees of rotation"].get<double>() == doctest::Approx(-1.5).epsilon(1e-3));
    CHECK(p["Output DPI"].get<double>() == doctest::Approx(970).epsilon(1e-3));
    CHECK(out["version"] == "1.7.0");
    CHECK(out["devices"].is_array());
}

TEST_CASE("existing Raw Accel values are composed, not overwritten") {
    RawAccelProfile c;
    c.yx_ratio = 1.05;
    std::string err;
    auto r = patch_rawaccel_settings(minimal_settings(0, 1.10, 1600).dump(), c, "", &err);
    REQUIRE(r);
    json out = json::parse(r->json);
    CHECK(out["profiles"][0]["Y/X output DPI ratio (vertical sens multiplier)"].get<double>() ==
          doctest::Approx(1.10 * 1.05).epsilon(1e-4));
    CHECK(out["profiles"][0]["Output DPI"].get<double>() == doctest::Approx(1600).epsilon(1e-3));
}

TEST_CASE("acceleration in the existing profile is reported") {
    std::string err;
    auto r = patch_rawaccel_settings(minimal_settings(0, 1, 1000, "classic").dump(), RawAccelProfile{}, "", &err);
    REQUIRE(r);
    CHECK_FALSE(r->warnings.empty());
}

TEST_CASE("non Raw Accel files are refused") {
    std::string err;
    CHECK_FALSE(patch_rawaccel_settings("{\"hello\": 1}", RawAccelProfile{}, "", &err));
    CHECK_FALSE(patch_rawaccel_settings("not json", RawAccelProfile{}, "", &err));
    json old = minimal_settings();
    old["profiles"][0].erase("Output DPI");
    CHECK_FALSE(patch_rawaccel_settings(old.dump(), RawAccelProfile{}, "", &err));
    CHECK(err.find("Output DPI") != std::string::npos);
}

TEST_CASE("a speed curve is written as Raw Accel's lookup table in sensitivity mode") {
    RawAccelProfile c;
    c.speed.points = {{0.5, 0.96}, {2.0, 1.0}, {6.0, 1.04}, {12.0, 1.04}};
    std::string err;
    auto r = patch_rawaccel_settings(minimal_settings().dump(), c, "", &err);
    REQUIRE_MESSAGE(r, err);
    CHECK(r->warnings.empty());
    const json p = json::parse(r->json)["profiles"][0];
    const json& ax = p["Whole or horizontal accel parameters"];
    CHECK(ax["mode"] == "lut");
    CHECK(ax["Gain / Velocity"] == false);  // y is the sensitivity multiplier itself
    REQUIRE(ax["data"].size() == 8);       // x0, y0, x1, y1, ...
    CHECK(ax["data"][0].get<double>() == doctest::Approx(0.5));
    CHECK(ax["data"][1].get<double>() == doctest::Approx(0.96));
    CHECK(ax["data"][6].get<double>() == doctest::Approx(12.0));
    CHECK(r->written.speed.points.size() == 4);

    // With a DPI set for the device, Raw Accel's speed unit is inches per second: speeds are converted.
    json s = minimal_settings();
    s["defaultDeviceConfig"]["DPI (normalizes input speed unit: counts/ms -> in/s)"] = 800;
    r = patch_rawaccel_settings(s.dump(), c, "", &err);
    REQUIRE(r);
    const json ax2 = json::parse(r->json)["profiles"][0]["Whole or horizontal accel parameters"];
    CHECK(ax2["data"][0].get<double>() == doctest::Approx(0.5 * 1000.0 / 800.0));
    CHECK(ax2["data"][1].get<double>() == doctest::Approx(0.96));

    // A profile that already uses acceleration keeps it; the speed part is left out, with a warning.
    r = patch_rawaccel_settings(minimal_settings(0, 1, 1000, "classic").dump(), c, "", &err);
    REQUIRE(r);
    CHECK(json::parse(r->json)["profiles"][0]["Whole or horizontal accel parameters"]["mode"] == "classic");
    CHECK(r->written.speed.empty());
    bool said = false;
    for (const auto& w : r->warnings) said = said || w.find("speed part") != std::string::npos;
    CHECK(said);
}
