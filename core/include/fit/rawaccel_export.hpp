// Export of a Raw Accel-compatible MouseFit profile.
//
// FitSuite never installs or configures Raw Accel itself. It shows the values to enter in Raw Accel's
// GUI, or writes a *new* settings file derived from the user's own Raw Accel settings.json (the
// original is never modified). Because FitSuite reads raw input *after* any active Raw Accel
// processing, a calibration made with Raw Accel active measured "your current Raw Accel settings";
// the export therefore composes the correction with the existing values.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fit/mouse_transform.hpp"

namespace fit {

struct RawAccelField {
    std::string gui_name;   // as shown in Raw Accel's GUI / settings.json key
    std::string value;
};

// Values to type into Raw Accel (assuming its other settings are at defaults / no acceleration).
std::vector<RawAccelField> rawaccel_fields(const RawAccelProfile& p);

// Best Raw Accel-family approximation of applying `correction` after `existing`.
// `max_error` receives the largest per-direction output error (fraction of input length).
RawAccelProfile compose_rawaccel(const RawAccelProfile& existing, const RawAccelProfile& correction, double* max_error);

struct RawAccelPatchResult {
    std::string json;                  // new settings.json text
    RawAccelProfile existing;          // what the file had before
    RawAccelProfile written;           // what the new file contains
    double composition_error = 0.0;
    std::vector<std::string> warnings; // acceleration in use, angle snapping, speed cap, …
};

// Reads a Raw Accel 1.7 settings.json, applies the correction to the named profile (first profile
// when empty) and returns the new document. Returns nullopt with `error` if the file is not a
// Raw Accel settings file or a required field is missing.
std::optional<RawAccelPatchResult> patch_rawaccel_settings(std::string_view settings_json, const RawAccelProfile& correction,
                                                           const std::string& profile_name, std::string* error);

}  // namespace fit
