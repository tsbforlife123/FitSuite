// JSON (de)serialisation with validation. Every reader reports problems with a JSON path, e.g.
// "hardware.mouse.dpi: expected a number between 50 and 64000", and nothing invalid is accepted.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "fit/mouse_types.hpp"
#include "fit/profile.hpp"
#include "nlohmann/json.hpp"

namespace fit {

using json = nlohmann::json;

inline constexpr const char* kSetupSchema = "fitsuite.setup";
inline constexpr const char* kSessionSchema = "fitsuite.mousefit.session";
inline constexpr int kSessionSchemaVersion = 2;  // 2: round-based sessions (format, rounds, tracking)
inline constexpr const char* kMouseProfileSchema = "fitsuite.mouseprofile";
inline constexpr int kMouseProfileSchemaVersion = 1;

json to_json(const SetupProfile& p);
std::optional<SetupProfile> setup_from_json(const json& j, std::vector<std::string>& errors);

json to_json(const Session& s);
std::optional<Session> session_from_json(const json& j, std::vector<std::string>& errors);

json to_json(const Transform& t);
std::optional<Transform> transform_from_json(const json& j, const std::string& path, std::vector<std::string>& errors);

json to_json(const MouseProfileRecord& r);
// Stand-alone export document for sharing a mouse profile between installs.
json export_mouse_profile(const MouseProfileRecord& r);
std::optional<MouseProfileRecord> import_mouse_profile(const json& j, std::vector<std::string>& errors);

// Validates a transform against the hard safety limits (bounded gain, bounded angle, strength 0..1).
bool transform_within_limits(const Transform& t, std::string* why);

}  // namespace fit
