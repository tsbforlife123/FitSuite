// Profile storage: one folder, atomic writes, rotating backups, validated loads.
//
//   <root>/setup.json                 current shared setup profile
//   <root>/backups/setup-*.json       previous versions (newest 10 kept)
//   <root>/recordings/<id>.json       MouseFit sessions with every raw input report
//   <root>/framefit/<run>.csv         copies of PresentMon captures
//   <root>/exports/                   files the user exported
//
// A file is never overwritten in place: data is written to a temporary file, flushed to disk and
// then renamed over the target, so a crash leaves either the old or the new version. A setup file
// that fails validation is kept (renamed *.corrupt-*), never deleted.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fit/mouse_types.hpp"
#include "fit/profile.hpp"

namespace fit {

bool atomic_write_file(const std::filesystem::path& target, std::string_view data, std::string* error);
std::optional<std::string> read_file(const std::filesystem::path& p, std::string* error, size_t max_bytes = 512u << 20);

struct LoadOutcome {
    std::optional<SetupProfile> setup;
    bool created_new = false;
    bool recovered_from_backup = false;
    std::string recovered_from;          // backup file name
    std::string corrupt_kept_as;         // where the unreadable file was moved
    std::vector<std::string> errors;     // validation problems of the primary file
};

class Store {
public:
    explicit Store(std::filesystem::path root);

    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path setup_path() const { return root_ / "setup.json"; }
    std::filesystem::path backups_dir() const { return root_ / "backups"; }
    std::filesystem::path recordings_dir() const { return root_ / "recordings"; }
    std::filesystem::path framefit_dir() const { return root_ / "framefit"; }
    std::filesystem::path exports_dir() const { return root_ / "exports"; }
    std::filesystem::path session_path(const std::string& id) const;

    bool ensure_dirs(std::string* error) const;

    LoadOutcome load_setup() const;
    // Sets updated_utc, backs up the previous file, writes atomically.
    bool save_setup(SetupProfile& p, std::string* error) const;

    bool save_session(const Session& s, std::string* error) const;
    std::optional<Session> load_session(const std::string& id, std::vector<std::string>& errors) const;

    bool export_mouse_profile(const MouseProfileRecord& r, const std::filesystem::path& file, std::string* error) const;
    std::optional<MouseProfileRecord> import_mouse_profile_file(const std::filesystem::path& file,
                                                                std::vector<std::string>& errors) const;

    std::vector<std::filesystem::path> list_backups() const;  // newest first

private:
    std::filesystem::path root_;
    size_t keep_backups_ = 10;
};

}  // namespace fit
