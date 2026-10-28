#include "fit/update.hpp"

#include <cctype>
#include <cstdio>

#include "nlohmann/json.hpp"

namespace fit {

Version parse_version(const std::string& s) {
    Version v;
    size_t i = 0;
    while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    int parts[3] = {0, 0, 0};
    int n = 0;
    while (n < 3 && i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        long x = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) && x < 100000) x = x * 10 + (s[i++] - '0');
        parts[n++] = int(x);
        if (i < s.size() && s[i] == '.') ++i;
        else break;
    }
    if (n == 0) return v;
    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];
    v.ok = true;
    return v;
}

int compare_versions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

std::optional<ReleaseInfo> parse_github_release(const std::string& text, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<ReleaseInfo> {
        if (error) *error = why;
        return std::nullopt;
    };
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return fail("The update information could not be read.");
    if (j.contains("message") && !j.contains("tag_name"))
        return fail("No release found (" + j.value("message", std::string("unknown")) + ").");
    ReleaseInfo r;
    r.tag = j.value("tag_name", std::string());
    r.version = parse_version(r.tag);
    if (!r.version.ok) return fail("The latest release has no version number.");
    r.name = j.value("name", std::string());
    r.notes = j.value("body", std::string());
    if (r.notes.size() > 4000) r.notes = r.notes.substr(0, 4000) + "…";
    r.published_utc = j.value("published_at", std::string());
    r.prerelease = j.value("prerelease", false);
    if (j.contains("assets") && j["assets"].is_array())
        for (const auto& a : j["assets"]) {
            if (!a.is_object()) continue;
            const std::string n = a.value("name", std::string());
            const std::string url = a.value("browser_download_url", std::string());
            if (url.rfind("https://", 0) != 0) continue;
            if (n == kUpdateExeAsset) {
                r.exe_url = url;
                r.exe_size = a.value("size", 0LL);
            } else if (n == kUpdateShaAsset) {
                r.sha_url = url;
            }
        }
    if (r.exe_url.empty() || r.sha_url.empty())
        return fail("The latest release (" + r.tag + ") does not include " + kUpdateExeAsset + " and its checksum.");
    return r;
}

std::optional<std::string> parse_sha256_text(const std::string& text) {
    size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (text.size() - i < 64) return std::nullopt;
    std::string h;
    for (size_t k = 0; k < 64; ++k) {
        const char c = char(std::tolower(static_cast<unsigned char>(text[i + k])));
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
        h += c;
    }
    if (i + 64 < text.size() && !std::isspace(static_cast<unsigned char>(text[i + 64]))) return std::nullopt;
    return h;
}

}  // namespace fit
