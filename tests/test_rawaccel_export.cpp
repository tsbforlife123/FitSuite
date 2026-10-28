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
