// Updates and downloads: FitSuite's own updates from its public GitHub releases, and the pinned
// Raw Accel release the setup can install. Only the parsing and checking live here; the platform
// layer does the network and file work.
#pragma once

#include <optional>
#include <string>

namespace fit {

// Where updates come from: the latest release of this public repository.
inline constexpr const char* kUpdateRepo = "tsbforlife123/FitSuite";
inline constexpr const char* kUpdateApiUrl = "https://api.github.com/repos/tsbforlife123/FitSuite/releases/latest";
inline constexpr const char* kReleasesPage = "https://github.com/tsbforlife123/FitSuite/releases";
// Every release carries the program and its SHA-256, published together.
inline constexpr const char* kUpdateExeAsset = "FitSuite.exe";
inline constexpr const char* kUpdateShaAsset = "FitSuite.exe.sha256";

// Raw Accel, as installed by the setup: the official release zip, pinned by version and SHA-256 so
// a changed or tampered download is refused.
inline constexpr const char* kRawAccelVersion = "1.7.1";
inline constexpr const char* kRawAccelZipUrl =
    "https://github.com/RawAccelOfficial/rawaccel/releases/download/v1.7.1/RawAccel_v1.7.1.zip";
inline constexpr const char* kRawAccelZipSha256 = "770fe3ae0919ca3c4d412f58c985eb27f5434decad809f7e8206de4e8852eec4";
inline constexpr const char* kRawAccelReleasesPage = "https://github.com/RawAccelOfficial/rawaccel/releases";
// Raw Accel needs the Visual C++ 2015–2022 runtime (x64).
inline constexpr const char* kVcRuntimeUrl = "https://aka.ms/vs/17/release/vc_redist.x64.exe";

struct Version {
    int major = 0, minor = 0, patch = 0;
    bool ok = false;
};
Version parse_version(const std::string& s);  // "v0.2.0", "0.2.0", "0.2.0 (preview)"
int compare_versions(const Version& a, const Version& b);  // <0, 0, >0

struct ReleaseInfo {
    std::string tag;          // "v0.2.0"
    Version version;
    std::string name;
    std::string notes;        // release text (shown to the player)
    std::string published_utc;
    std::string exe_url;      // FitSuite.exe
    std::string sha_url;      // FitSuite.exe.sha256
    long long exe_size = 0;
    bool prerelease = false;
    // The version as players see it: "1.0.0" (no "v", no "FitSuite").
    std::string version_text() const {
        return std::to_string(version.major) + "." + std::to_string(version.minor) + "." + std::to_string(version.patch);
    }
};

// GitHub's "latest release" JSON. nullopt (with *error) when it is not usable.
std::optional<ReleaseInfo> parse_github_release(const std::string& json, std::string* error);

// "<64 hex>  FitSuite.exe" (sha256sum format) or just the hash. Lower-case hex, or nullopt.
std::optional<std::string> parse_sha256_text(const std::string& text);

}  // namespace fit
