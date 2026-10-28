#include "fit/store.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "fit/serialize.hpp"
#include "fit/timeutil.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace fit {

bool atomic_write_file(const fs::path& target, std::string_view data, std::string* error) {
    auto fail = [&](const std::string& e) {
        if (error) *error = e;
        return false;
    };
    std::error_code ec;
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
    const fs::path tmp = target.string() + ".tmp-" + make_id("w").substr(2, 8);
#ifdef _WIN32
    HANDLE h = CreateFileW(tmp.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("Could not create " + tmp.string() + " (error " + std::to_string(GetLastError()) + ").");
    size_t off = 0;
    while (off < data.size()) {
        DWORD wrote = 0;
        const DWORD chunk = DWORD(std::min<size_t>(data.size() - off, 1u << 24));
        if (!WriteFile(h, data.data() + off, chunk, &wrote, nullptr) || wrote == 0) {
            CloseHandle(h);
            DeleteFileW(tmp.wstring().c_str());
            return fail("Writing " + tmp.string() + " failed (disk full?).");
        }
        off += wrote;
    }
    if (!FlushFileBuffers(h)) {
        CloseHandle(h);
        DeleteFileW(tmp.wstring().c_str());
        return fail("Flushing " + tmp.string() + " to disk failed.");
    }
    CloseHandle(h);
    if (!MoveFileExW(tmp.wstring().c_str(), target.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError();
        DeleteFileW(tmp.wstring().c_str());
        return fail("Replacing " + target.string() + " failed (error " + std::to_string(e) + "). The previous version is unchanged.");
    }
    return true;
#else
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fail("Could not create " + tmp.string() + ".");
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t w = ::write(fd, data.data() + off, data.size() - off);
        if (w <= 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            return fail("Writing " + tmp.string() + " failed (disk full?).");
        }
        off += size_t(w);
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlink(tmp.c_str());
        return fail("Flushing " + tmp.string() + " to disk failed.");
    }
    ::close(fd);
    if (::rename(tmp.c_str(), target.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return fail("Replacing " + target.string() + " failed. The previous version is unchanged.");
    }
    const int dfd = ::open(target.parent_path().empty() ? "." : target.parent_path().c_str(), O_RDONLY);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return true;
#endif
}

std::optional<std::string> read_file(const fs::path& p, std::string* error, size_t max_bytes) {
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec) {
        if (error) *error = "Cannot read " + p.string() + ": " + ec.message();
        return std::nullopt;
    }
    if (size > max_bytes) {
        if (error) *error = p.string() + " is too large to be a FitSuite file.";
        return std::nullopt;
    }
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        if (error) *error = "Cannot open " + p.string() + ".";
        return std::nullopt;
    }
    std::string s;
    s.resize(size_t(size));
    in.read(s.data(), std::streamsize(size));
    if (!in && !in.eof()) {
        if (error) *error = "Reading " + p.string() + " failed.";
        return std::nullopt;
    }
    return s;
}

Store::Store(fs::path root) : root_(std::move(root)) {}

fs::path Store::session_path(const std::string& id) const {
    std::string safe;
    for (char c : id)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') safe += c;
    return recordings_dir() / (safe + ".json");
}

bool Store::ensure_dirs(std::string* error) const {
    std::error_code ec;
    for (const auto& d : {root_, backups_dir(), recordings_dir(), framefit_dir(), exports_dir()}) {
        fs::create_directories(d, ec);
        if (ec) {
            if (error) *error = "Cannot create folder " + d.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

std::vector<fs::path> Store::list_backups() const {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(backups_dir(), ec)) return out;
    for (const auto& e : fs::directory_iterator(backups_dir(), ec)) {
        const auto name = e.path().filename().string();
        if (name.rfind("setup-", 0) == 0 && e.path().extension() == ".json") out.push_back(e.path());
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) { return a.filename() > b.filename(); });
    return out;
}

namespace {
std::optional<SetupProfile> parse_setup_file(const fs::path& p, std::vector<std::string>& errors) {
    std::string err;
    auto text = read_file(p, &err);
    if (!text) {
        errors.push_back(err);
        return std::nullopt;
    }
    json j = json::parse(*text, nullptr, false);
    if (j.is_discarded()) {
        errors.push_back(p.filename().string() + " is not valid JSON (the file may be damaged).");
        return std::nullopt;
    }
    return setup_from_json(j, errors);
}
}  // namespace

LoadOutcome Store::load_setup() const {
    LoadOutcome out;
    std::error_code ec;
    if (!fs::exists(setup_path(), ec)) {
        out.setup = make_default_setup();
        out.created_new = true;
        return out;
    }
    out.setup = parse_setup_file(setup_path(), out.errors);
    if (out.setup) return out;

    for (const auto& b : list_backups()) {
        std::vector<std::string> berr;
        if (auto s = parse_setup_file(b, berr)) {
            out.setup = s;
            out.recovered_from_backup = true;
            out.recovered_from = b.filename().string();
            break;
        }
    }
    // Keep the unreadable file for inspection; never delete user data.
    const fs::path keep = root_ / ("setup.json.corrupt-" + utc_compact_now());
    fs::rename(setup_path(), keep, ec);
    if (!ec) out.corrupt_kept_as = keep.filename().string();
    return out;
}

bool Store::save_setup(SetupProfile& p, std::string* error) const {
    if (!ensure_dirs(error)) return false;
    p.updated_utc = utc_now_iso();
    p.app_version = kAppVersion;
    std::error_code ec;
    if (fs::exists(setup_path(), ec)) {
        // Back up the previous version (copy, so the live file stays in place until the atomic swap).
        std::string stamp = utc_compact_now();
        fs::path dst = backups_dir() / ("setup-" + stamp + ".json");
        for (int i = 1; fs::exists(dst, ec); ++i) dst = backups_dir() / ("setup-" + stamp + "-" + std::to_string(i) + ".json");
        fs::copy_file(setup_path(), dst, fs::copy_options::overwrite_existing, ec);
        auto backups = list_backups();
        for (size_t i = keep_backups_; i < backups.size(); ++i) fs::remove(backups[i], ec);
    }
    return atomic_write_file(setup_path(), to_json(p).dump(2), error);
}

bool Store::save_session(const Session& s, std::string* error) const {
    if (!ensure_dirs(error)) return false;
    return atomic_write_file(session_path(s.id), to_json(s).dump(), error);
}

std::optional<Session> Store::load_session(const std::string& id, std::vector<std::string>& errors) const {
    std::string err;
    auto text = read_file(session_path(id), &err);
    if (!text) {
        errors.push_back(err);
        return std::nullopt;
    }
    json j = json::parse(*text, nullptr, false);
    if (j.is_discarded()) {
        errors.push_back("The session file is not valid JSON.");
        return std::nullopt;
    }
    return session_from_json(j, errors);
}

bool Store::export_mouse_profile(const MouseProfileRecord& r, const fs::path& file, std::string* error) const {
    return atomic_write_file(file, fit::export_mouse_profile(r).dump(2), error);
}

std::optional<MouseProfileRecord> Store::import_mouse_profile_file(const fs::path& file, std::vector<std::string>& errors) const {
    std::string err;
    auto text = read_file(file, &err, 4u << 20);
    if (!text) {
        errors.push_back(err);
        return std::nullopt;
    }
    json j = json::parse(*text, nullptr, false);
    if (j.is_discarded()) {
        errors.push_back("The file is not valid JSON.");
        return std::nullopt;
    }
    return fit::import_mouse_profile(j, errors);
}

}  // namespace fit
