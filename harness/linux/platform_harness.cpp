#include "platform_harness.hpp"

#include <algorithm>
#include <cstdio>

namespace fitapp {

void HarnessMouse::push(const RawEvent& e) {
    auto it = std::upper_bound(queue_.begin(), queue_.end(), e, [](const RawEvent& a, const RawEvent& b) { return a.t_us < b.t_us; });
    queue_.insert(it, e);
}

void HarnessMouse::drain(std::vector<RawEvent>& out) {
    while (!queue_.empty() && queue_.front().t_us <= *clock_) {
        if (active_) out.push_back(queue_.front());
        queue_.pop_front();
    }
}

std::vector<MouseDevice> HarnessMouse::devices() {
    MouseDevice d;
    d.id = "sim-mouse";
    d.name = "Simulated player (harness)";
    return {d};
}

std::optional<MouseDevice> HarnessMouse::device(const std::string& id) {
    if (id != "sim-mouse") return std::nullopt;
    return devices().front();
}

PresentMonStatus HarnessPresentMon::status() {
    PresentMonStatus s;
    s.available = false;
    s.note = "PresentMon runs on Windows only. In the development harness, import CSV files instead.";
    return s;
}

bool HarnessPresentMon::start(const CaptureRequest&, std::string* error) {
    if (error) *error = "PresentMon is not available in the Linux development harness.";
    return false;
}

HarnessPlatform::HarnessPlatform(std::filesystem::path data_dir) : data_dir_(std::move(data_dir)) {}

uint64_t HarnessPlatform::start_program(const std::filesystem::path& exe, const std::vector<std::filesystem::path>& args,
                                        std::string* error) {
    if (!rawaccel_present || exe.filename() != "writer.exe") {
        if (error) *error = "Only the simulated Raw Accel writer can run in the harness.";
        return 0;
    }
    char t[32];
    std::snprintf(t, sizeof t, "%.2f", double(clock_us) * 1e-6);
    writer_log.push_back(std::string(t) + " " + (args.empty() ? std::string("(no file)") : args.front().filename().string()));
    std::printf("writer.exe %s\n", writer_log.back().c_str());
    return ++next_program;
}

fit::PcInfo HarnessPlatform::detect_pc() {
    fit::PcInfo pc;  // nothing is detected in the harness
    pc.source = "not detected";
    return pc;
}

std::vector<DisplayInfo> HarnessPlatform::displays() {
    DisplayInfo d;
    d.device = "harness-display";
    d.name = "Virtual display (development harness)";
    d.width = 1600;
    d.height = 1000;
    d.refresh_hz = base_hz_;
    d.modes_hz = {60.0, 120.0, 144.0, 240.0};
    d.primary = true;
    d.hosts_window = true;
    d.ddcci = "unknown";
    return {d};
}

bool HarnessPlatform::set_refresh_temporary(const std::string&, double hz, std::string*) {
    current_hz_ = hz;
    return true;
}

bool HarnessPlatform::restore_display(const std::string&, std::string*) {
    current_hz_ = base_hz_;
    return true;
}

bool HarnessPlatform::set_refresh_persistent(const std::string&, double hz, std::string*) {
    base_hz_ = current_hz_ = hz;
    return true;
}

FrameTiming HarnessPlatform::frame_timing() const {
    FrameTiming t;
    t.last_interval_ms = frame_dt_ms;
    t.refresh_hz = current_hz_;
    t.tearing_supported = false;
    t.limiter_fps = limiter_;
    return t;
}

std::optional<std::filesystem::path> HarnessPlatform::open_file_dialog(const std::string&, const std::string&, const std::string&) {
    auto r = next_dialog_;
    next_dialog_.reset();
    return r;
}

std::optional<std::filesystem::path> HarnessPlatform::save_file_dialog(const std::string&, const std::string&, const std::string&,
                                                                       const std::string&) {
    auto r = next_dialog_;
    next_dialog_.reset();
    return r;
}

}  // namespace fitapp

namespace fitapp {

uint64_t HarnessPlatform::start_download(const std::string& url, const std::filesystem::path& dest, std::string* error) {
    namespace fs = std::filesystem;
    DownloadStatus st;
    st.done = true;
    auto it = fake_downloads.find(url);
    std::error_code ec;
    if (it == fake_downloads.end()) {
        st.error = "Offline (development harness): no stand-in for " + url;
    } else {
        fs::create_directories(dest.parent_path(), ec);
        fs::copy_file(it->second, dest, fs::copy_options::overwrite_existing, ec);
        st.ok = !ec;
        st.bytes = st.total = st.ok ? uint64_t(fs::file_size(dest, ec)) : 0;
        if (!st.ok) st.error = ec.message();
    }
    std::printf("download %s -> %s: %s\n", url.c_str(), dest.string().c_str(), st.ok ? "ok" : st.error.c_str());
    if (error && !st.ok && it == fake_downloads.end()) *error = st.error;
    const uint64_t token = ++next_program;
    downloads_[token] = st;
    return token;
}

IPlatform::DownloadStatus HarnessPlatform::download_status(uint64_t token) {
    auto it = downloads_.find(token);
    if (it == downloads_.end()) {
        DownloadStatus s;
        s.done = true;
        s.error = "Unknown download.";
        return s;
    }
    return it->second;
}

uint64_t HarnessPlatform::start_unzip(const std::filesystem::path& zip, const std::filesystem::path& dest, std::string*) {
    std::filesystem::create_directories(dest);
    const std::string cmd = "unzip -oq '" + zip.string() + "' -d '" + dest.string() + "'";
    const int rc = std::system(cmd.c_str());
    std::printf("unzip %s -> %s: %d\n", zip.filename().string().c_str(), dest.string().c_str(), rc);
    const uint64_t token = ++next_program;
    exit_codes_[token] = rc == 0 ? 0 : 1;
    return token;
}

uint64_t HarnessPlatform::start_elevated(const std::filesystem::path& exe, std::string*) {
    elevated_log.push_back(exe.string());
    std::printf("elevated %s (exit %d)\n", exe.string().c_str(), elevated_exit_code);
    const uint64_t token = ++next_program;
    exit_codes_[token] = elevated_exit_code;
    return token;
}

std::vector<std::filesystem::path> HarnessPlatform::find_rawaccel_folders() {
    namespace fs = std::filesystem;
    std::vector<fs::path> out;
    std::error_code ec;
    for (const fs::path& d : {programs_dir() / "RawAccel", data_dir() / "harness-rawaccel"})
        if (fs::is_regular_file(d / "writer.exe", ec)) out.push_back(d);
    return out;
}

bool HarnessPlatform::apply_update(const std::filesystem::path& new_exe, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(new_exe, ec)) {
        if (error) *error = "The downloaded update is missing.";
        return false;
    }
    update_log.push_back(new_exe.string());
    std::printf("apply-update %s\n", new_exe.string().c_str());
    return true;
}

}  // namespace fitapp
