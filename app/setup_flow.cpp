// First-run setup helpers (installing Raw Accel) and FitSuite's own updates.
#include <fstream>
#include <sstream>

#include "app.hpp"
#include "fit/sha256.hpp"
#include "fit/store.hpp"
#include "fit/timeutil.hpp"

namespace fitapp {

using namespace fit;
namespace fs = std::filesystem;

namespace {

std::string read_small_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream os;
    os << f.rdbuf();
    std::string s = os.str();
    if (s.size() > (1u << 20)) s.resize(1u << 20);
    return s;
}


}  // namespace

// ---- Raw Accel ------------------------------------------------------------------------------------

fs::path App::rawaccel_folder() {
    if (clock_s() - rawaccel_folder_checked_at_ < 3.0) return rawaccel_folder_cache_;
    rawaccel_folder_checked_at_ = clock_s();
    if (setup_.app.rawaccel_install_pending && rawaccel_installed()) {  // the restart after installing happened
        setup_.app.rawaccel_install_pending = false;
        if (ra_install.step == RaInstall::Step::NeedsRestart) ra_install.step = RaInstall::Step::Idle;
        save();
    }
    std::error_code ec;
    fs::path found;
    const fs::path mine = setup_.app.rawaccel_folder;
    const fs::path chosen = rawaccel_settings_path();
    if (!mine.empty() && fs::is_regular_file(mine / "writer.exe", ec)) found = mine;
    else if (!chosen.empty() && fs::is_regular_file(chosen.parent_path() / "writer.exe", ec)) found = chosen.parent_path();
    else {
        const auto all = platform_.find_rawaccel_folders();
        for (const auto& d : all)  // prefer one that already has its settings file
            if (fs::is_regular_file(d / "settings.json", ec)) {
                found = d;
                break;
            }
        if (found.empty() && !all.empty()) found = all.front();
    }
    // Remember its settings.json once Raw Accel has written one.
    if (!found.empty() && chosen.empty() && fs::is_regular_file(found / "settings.json", ec))
        remember_rawaccel_settings_path(found / "settings.json");
    rawaccel_folder_cache_ = found;
    return found;
}

bool App::rawaccel_needs_vc_runtime() { return !platform_.vc_runtime_installed(); }

void App::open_rawaccel_gui() {
    const fs::path dir = rawaccel_folder();
    std::string err;
    if (dir.empty() || !platform_.start_program(dir / "rawaccel.exe", {}, &err))
        toast(ui::Tone::Bad, dir.empty() ? std::string("Raw Accel's folder was not found.") : err, 8.0);
    rawaccel_folder_checked_at_ = -100.0;
}

void App::ra_install_start() {
    if (ra_install.token) return;
    ra_install = RaInstall{};
    ra_install.zip = platform_.data_dir() / "downloads" / ("RawAccel_v" + std::string(kRawAccelVersion) + ".zip");
    std::string err;
    // Already downloaded earlier (and still intact)? Then go straight to unpacking.
    if (auto h = sha256_file(ra_install.zip); h && *h == kRawAccelZipSha256) {
        ra_install.token = platform_.start_unzip(ra_install.zip, platform_.programs_dir(), &err);
        ra_install.step = ra_install.token ? RaInstall::Step::Unzipping : RaInstall::Step::Failed;
        ra_install.message = err;
        return;
    }
    ra_install.token = platform_.start_download(kRawAccelZipUrl, ra_install.zip, &err);
    ra_install.step = ra_install.token ? RaInstall::Step::Downloading : RaInstall::Step::Failed;
    ra_install.message = err;
}

void App::tick_ra_install() {
    auto& r = ra_install;
    if (!r.token) return;
    std::string err;
    std::error_code ec;
    switch (r.step) {
        case RaInstall::Step::Downloading: {
            const auto st = platform_.download_status(r.token);
            r.bytes = st.bytes;
            r.total = st.total;
            if (!st.done) return;
            r.token = 0;
            if (!st.ok) {
                r.step = RaInstall::Step::Failed;
                r.message = "The download failed: " + st.error;
                return;
            }
            const auto h = sha256_file(r.zip);
            if (!h || *h != kRawAccelZipSha256) {
                fs::remove(r.zip, ec);
                r.step = RaInstall::Step::Failed;
                r.message = std::string("The download did not match the official Raw Accel ") + kRawAccelVersion +
                            " release, so it was not used.";
                return;
            }
            r.token = platform_.start_unzip(r.zip, platform_.programs_dir(), &err);
            r.step = r.token ? RaInstall::Step::Unzipping : RaInstall::Step::Failed;
            r.message = err;
            return;
        }
        case RaInstall::Step::Unzipping: {
            const auto code = platform_.program_exit_code(r.token);
            if (!code) return;
            r.token = 0;
            r.folder = platform_.programs_dir() / "RawAccel";
            if (*code != 0 || !fs::is_regular_file(r.folder / "installer.exe", ec)) {
                r.step = RaInstall::Step::Failed;
                r.message = "Raw Accel could not be unpacked into " + platform_.programs_dir().string() + ".";
                return;
            }
            setup_.app.rawaccel_folder = r.folder.string();
            rawaccel_folder_checked_at_ = -100.0;
            save_now();
            r.token = platform_.start_elevated(r.folder / "installer.exe", &err);
            r.step = r.token ? RaInstall::Step::Installing : RaInstall::Step::Failed;
            r.message = err;
            return;
        }
        case RaInstall::Step::Installing: {
            const auto code = platform_.program_exit_code(r.token);
            if (!code) return;
            r.token = 0;
            if (*code != 0) {
                r.step = RaInstall::Step::Failed;
                r.message = ui::fmt("Raw Accel's installer reported a problem (code %d). Read its message, then try again.", *code);
                return;
            }
            r.step = RaInstall::Step::NeedsRestart;
            setup_.app.rawaccel_install_pending = true;
            save_now();
            return;
        }
        default: r.token = 0; return;
    }
}

// ---- updates --------------------------------------------------------------------------------------

void App::update_check(bool manual) {
    if (update_busy()) return;
    updater = Updater{};
    updater.manual = manual;
    std::string err;
    const fs::path dest = platform_.data_dir() / "update" / "latest-release.json";
    updater.token = platform_.start_download(kUpdateApiUrl, dest, &err);
    setup_.app.last_update_check_utc = utc_now_iso();
    save();
    if (!updater.token) {
        updater.step = manual ? Updater::Step::Failed : Updater::Step::Idle;
        updater.message = "Could not check for updates: " + err;
        return;
    }
    updater.step = Updater::Step::Checking;
}

void App::update_download() {
    if (updater.step != Updater::Step::Available || !updater.release) return;
    std::string err;
    updater.phase = 0;
    updater.token = platform_.start_download(updater.release->sha_url, platform_.data_dir() / "update" / "FitSuite.exe.sha256", &err);
    updater.step = updater.token ? Updater::Step::Downloading : Updater::Step::Failed;
    updater.message = updater.token ? std::string() : "The update could not be downloaded: " + err;
}

void App::update_install() {
    if (updater.step != Updater::Step::Ready) return;
    std::string err;
    save_now();
    if (platform_.apply_update(updater.exe, &err)) {
        updater.message = "Restarting into the new version…";
        platform_.request_quit();
    } else {
        updater.step = Updater::Step::Failed;
        updater.message = err;
    }
}

std::string App::take_update_notice() {
    if (updater.step != Updater::Step::Available || !updater.release) return "";
    if (setup_.app.update_notified == updater.release->tag) return "";
    setup_.app.update_notified = updater.release->tag;
    save();
    return "FitSuite " + updater.release->tag + " is available. Open FitSuite, then Settings → Updates to install it.";
}

void App::tick_updater() {
    auto& u = updater;
    // Daily check, when the player switched it on (the harness never goes online by itself).
    if (!u.token && setup_.app.update_check && !platform_.dev_harness() &&
        (u.step == Updater::Step::Idle || u.step == Updater::Step::UpToDate || u.step == Updater::Step::Failed)) {
        int64_t last = 0;
        if (!parse_utc_iso(setup_.app.last_update_check_utc, last) || unix_now() - last > 24 * 3600) update_check(false);
    }
    if (!u.token) return;
    const auto st = platform_.download_status(u.token);
    u.bytes = st.bytes;
    u.total = st.total;
    if (!st.done) return;
    u.token = 0;
    std::error_code ec;
    if (u.step == Updater::Step::Checking) {
        if (!st.ok) {
            u.step = u.manual ? Updater::Step::Failed : Updater::Step::Idle;
            u.message = "Could not check for updates: " + st.error;
            return;
        }
        std::string err;
        const auto rel = parse_github_release(read_small_file(platform_.data_dir() / "update" / "latest-release.json"), &err);
        if (!rel) {
            u.step = u.manual ? Updater::Step::Failed : Updater::Step::Idle;
            u.message = err;
            return;
        }
        u.release = rel;
        if (compare_versions(rel->version, parse_version(kAppVersion)) > 0) {
            u.step = Updater::Step::Available;
            toast(ui::Tone::Info, "FitSuite " + rel->tag + " is available: Settings → Updates.", 10.0);
        } else {
            u.step = Updater::Step::UpToDate;
        }
        return;
    }
    if (u.step == Updater::Step::Downloading && u.phase == 0) {
        if (!st.ok) {
            u.step = Updater::Step::Failed;
            u.message = "The update could not be downloaded: " + st.error;
            return;
        }
        const auto h = parse_sha256_text(read_small_file(platform_.data_dir() / "update" / "FitSuite.exe.sha256"));
        if (!h) {
            u.step = Updater::Step::Failed;
            u.message = "The update's checksum file could not be read, so the update was not downloaded.";
            return;
        }
        u.expected_sha = *h;
        u.phase = 1;
        u.exe = platform_.data_dir() / "update" / ("FitSuite-" + u.release->tag + ".exe");
        std::string err;
        u.token = platform_.start_download(u.release->exe_url, u.exe, &err);
        if (!u.token) {
            u.step = Updater::Step::Failed;
            u.message = "The update could not be downloaded: " + err;
        }
        return;
    }
    if (u.step == Updater::Step::Downloading && u.phase == 1) {
        if (!st.ok) {
            u.step = Updater::Step::Failed;
            u.message = "The update could not be downloaded: " + st.error;
            return;
        }
        const auto h = sha256_file(u.exe);
        if (!h || *h != u.expected_sha) {
            fs::remove(u.exe, ec);
            u.step = Updater::Step::Failed;
            u.message = "The downloaded update did not match its published checksum, so it was not used.";
            return;
        }
        u.step = Updater::Step::Ready;
    }
}

void App::finish_setup() {
    setup_.app.setup_done = true;
    setup_.app.setup_step = 0;
    save_now();
}

}  // namespace fitapp
