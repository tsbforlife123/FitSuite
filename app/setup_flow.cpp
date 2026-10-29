// First-run setup helpers (installing Raw Accel) and FitSuite's own updates.
#include <fstream>
#include <sstream>

#include "app.hpp"
#include "fit/rawaccel_export.hpp"
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

fs::path App::rawaccel_settings_ready(std::string* note) {
    fs::path sj = rawaccel_settings_path();
    if (!sj.empty()) return sj;
    rawaccel_folder_checked_at_ = -100.0;
    const fs::path dir = rawaccel_folder();  // also remembers a settings.json Raw Accel already saved
    sj = rawaccel_settings_path();
    if (!sj.empty() || dir.empty()) return sj;
    const fs::path p = dir / "settings.json";
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        std::string err;
        if (!atomic_write_file(p, default_rawaccel_settings_json(), &err)) {
            if (note) *note = "Could not write Raw Accel's settings file in " + dir.string() + ": " + err;
            return {};
        }
        AppliedChange ch;
        ch.id = make_id("ch");
        ch.kind = "rawaccel.settings_created";
        ch.target = p.string();
        ch.before = "no settings.json (Raw Accel had not saved one yet)";
        ch.after = "Raw Accel's default settings";
        ch.applied_utc = utc_now_iso();
        ch.status = "applied";
        setup_.applied_changes.push_back(std::move(ch));
        save();
        if (note) *note = "Raw Accel had no settings file yet, so FitSuite wrote Raw Accel's default one.";
    }
    remember_rawaccel_settings_path(p);
    return p;
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
    const bool dismissed = updater.prompt_dismissed;  // "Not now" lasts until FitSuite is opened again
    updater = Updater{};
    updater.manual = manual;
    updater.prompt_dismissed = dismissed;
    checked_this_launch_ = true;
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
    return "FitSuite version " + updater.release->version_text() + " is available. Open FitSuite to update.";
}

void App::tick_updater() {
    auto& u = updater;
    // Checked every time FitSuite opens, and again once a day while it keeps running (in the
    // notification area, say), when the player switched it on. The harness never goes online by itself.
    if (!u.token && setup_.app.update_check && !platform_.dev_harness() &&
        (u.step == Updater::Step::Idle || u.step == Updater::Step::UpToDate || u.step == Updater::Step::Failed)) {
        int64_t last = 0;
        if (!checked_this_launch_ || !parse_utc_iso(setup_.app.last_update_check_utc, last) || unix_now() - last > 24 * 3600)
            update_check(false);
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
            // Normally the "Update available" box asks; during the first-run setup a short note does.
            if (!setup_.app.setup_done) toast(ui::Tone::Info, "Version " + rel->version_text() + " is available: Settings → Updates.", 10.0);
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

// The "Update available" box, in the middle of the window, when FitSuite opens and a newer version
// exists. "Update" downloads it, checks it and restarts into it; "Not now" hides the box until
// FitSuite is opened again (Settings → Updates still has it). Never shown during a test or the
// first-run setup.
void App::draw_update_prompt() {
    auto& u = updater;
    if (!setup_.app.setup_done || u.prompt_dismissed || !u.release || mouse.test || motion.test) return;
    if (whats_new_showing()) return;  // "What's new" first
    const bool working = u.install_when_ready;  // the player clicked Update
    if (!working && u.step != Updater::Step::Available) return;
    if (working && u.step == Updater::Step::Ready) {
        u.install_when_ready = false;  // only once: update_install() restarts FitSuite
        update_install();
        return;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480, 0));
    ImGui::Begin("Update available", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoMove);
    ui::wrapped(ui::fmt("Version %s is available. You have version %s.", u.release->version_text().c_str(), kAppVersion).c_str());
    if (!u.release->notes.empty()) {
        std::string notes = u.release->notes;
        if (notes.size() > 500) notes = notes.substr(0, 500) + "…";
        ImGui::Spacing();
        ui::wrapped(notes.c_str(), theme::kTextMuted);
    }
    ImGui::Spacing();
    if (u.step == Updater::Step::Failed) {
        ui::callout(ui::Tone::Bad, "The update did not work", u.message.c_str());
        if (ui::button_secondary("Close")) {
            u.install_when_ready = false;
            u.prompt_dismissed = true;
        }
    } else if (working) {
        const float frac = u.total ? float(double(u.bytes) / double(u.total)) : 0.0f;
        ui::progress_bar(frac, "");
        ui::faint("Downloading and checking the update. FitSuite restarts by itself when it is ready.");
    } else {
        ui::faint("Your settings and measurements stay as they are. FitSuite restarts into the new version.");
        ImGui::Spacing();
        if (ui::button_primary("Update")) {
            u.install_when_ready = true;
            update_download();
        }
        ImGui::SameLine();
        if (ui::button_secondary("Not now")) u.prompt_dismissed = true;
    }
    ImGui::End();
}

// The release notes as plain text: Markdown's **bold** and `code` marks are left out.
static std::string plain_notes(const char* md) {
    std::string out;
    for (const char* p = md; *p; ++p) {
        if (*p == '`') continue;
        if (p[0] == '*' && p[1] == '*') { ++p; continue; }
        out += *p;
    }
    return out;
}

// "What's new": this version's notes, shown each time FitSuite opens (once the setup is done) until
// the player clicks "Don't show again". That lasts until the next update, whose notes show again.
// "What's new" in the status bar opens it any time.
bool App::whats_new_showing() {
    if (!*kReleaseNotes || mouse.test || motion.test) return false;
    if (whats_new_open_) return true;
    return setup_.app.setup_done && !whats_new_closed_ && setup_.app.notes_hidden != kAppVersion;
}

void App::draw_whats_new() {
    if (!whats_new_showing()) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520, 0));
    const std::string title = std::string("What's new in version ") + kAppVersion;
    ImGui::Begin(title.c_str(), nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoMove);
    ui::wrapped(plain_notes(kReleaseNotes).c_str());
    ImGui::Spacing();
    if (ui::button_primary("Close")) {
        whats_new_open_ = false;
        whats_new_closed_ = true;
    }
    if (setup_.app.notes_hidden != kAppVersion) {
        ImGui::SameLine(0, 12);
        if (ui::button_secondary("Don't show again")) {
            whats_new_open_ = false;
            whats_new_closed_ = true;
            setup_.app.notes_hidden = kAppVersion;
            save_now();
        }
    }
    ImGui::Dummy(ImVec2(0, 2));
    ui::faint(setup_.app.notes_hidden == kAppVersion
                  ? "Hidden until the next update. \"What's new\" in the bottom-right corner opens it any time."
                  : "Shown each time FitSuite opens until you click \"Don't show again\".");
    ImGui::End();
}

void App::finish_setup() {
    whats_new_closed_ = true;  // not straight after the setup: from the next time FitSuite opens
    setup_.app.setup_done = true;
    setup_.app.setup_step = 0;
    save_now();
}

}  // namespace fitapp
