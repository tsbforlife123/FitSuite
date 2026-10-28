#include "win32_platform.hpp"

#include <shellapi.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "fit/store.hpp"

namespace fs = std::filesystem;

namespace fitapp::win32 {

namespace {

fs::path exe_dir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return n ? fs::path(std::wstring(buf, n)).parent_path() : fs::current_path();
}

fs::path newest_matching(const fs::path& dir, const std::string& prefix, const std::string& ext) {
    fs::path best;
    fs::file_time_type best_t{};
    std::error_code ec;
    if (!fs::exists(dir, ec)) return best;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = narrow(e.path().filename().wstring());
        if (n.rfind(prefix, 0) != 0 || e.path().extension() != ext) continue;
        const auto t = e.last_write_time(ec);
        if (best.empty() || t > best_t) {
            best = e.path();
            best_t = t;
        }
    }
    return best;
}

}  // namespace

PresentMonRunner::PresentMonRunner(fs::path config_dir) : config_dir_(std::move(config_dir)) {
    std::string err;
    if (auto s = fit::read_file(config_dir_ / "presentmon-path.txt", &err, 4096)) {
        std::string p = *s;
        while (!p.empty() && (p.back() == '\n' || p.back() == '\r' || p.back() == ' ')) p.pop_back();
        user_path_ = fs::path(widen(p));
    }
}

PresentMonRunner::~PresentMonRunner() {
    if (process_) {
        TerminateProcess(process_, 1);
        CloseHandle(process_);
    }
}

fs::path PresentMonRunner::find_executable() const {
    std::error_code ec;
    if (!user_path_.empty() && fs::exists(user_path_, ec)) return user_path_;
    for (const auto& dir : {exe_dir(), exe_dir() / "tools", config_dir_ / "tools"}) {
        const fs::path p = newest_matching(dir, "PresentMon", ".exe");
        if (!p.empty()) return p;
    }
    return {};
}

PresentMonStatus PresentMonRunner::status() {
    PresentMonStatus s;
    const fs::path p = find_executable();
    s.available = !p.empty();
    s.path = narrow(p.wstring());
    s.note = s.available
                 ? "Needs administrator rights, or your account in the \"Performance Log Users\" group (then sign out and in)."
                 : "Download the PresentMon console application (PresentMon-2.x.x-x64.exe) and choose it here, or put it in "
                   "a \"tools\" folder next to FitSuite.exe.";
    return s;
}

void PresentMonRunner::set_path(const std::string& path) {
    user_path_ = fs::path(widen(path));
    std::string err;
    fit::atomic_write_file(config_dir_ / "presentmon-path.txt", path, &err);
}

bool PresentMonRunner::start(const CaptureRequest& req, std::string* error) {
    if (process_) {
        if (error) *error = "A capture is already running.";
        return false;
    }
    const fs::path exe = find_executable();
    if (exe.empty()) {
        if (error) *error = "PresentMon was not found.";
        return false;
    }
    req_ = req;
    finished_reported_ = false;
    std::error_code ec;
    fs::create_directories(req.output_csv.parent_path(), ec);
    std::wstringstream args;
    args << L"--process_name \"" << widen(req.process_name) << L"\" --output_file \"" << req.output_csv.wstring()
         << L"\" --timed " << req.duration_s << L" --terminate_after_timed --no_console_stats --stop_existing_session"
         << L" --session_name FitSuite";
    if (!req.hotkey.empty()) args << L" --hotkey " << widen(req.hotkey);
    if (req.delay_s > 0) args << L" --delay " << req.delay_s;

    if (req.elevate) {
        SHELLEXECUTEINFOW sei{sizeof sei};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"runas";
        const std::wstring file = exe.wstring();
        const std::wstring params = args.str();
        sei.lpFile = file.c_str();
        sei.lpParameters = params.c_str();
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei) || !sei.hProcess) {
            if (error) *error = GetLastError() == ERROR_CANCELLED ? "The administrator prompt was declined."
                                                                  : "PresentMon could not be started as administrator.";
            return false;
        }
        process_ = sei.hProcess;
        log_.clear();
    } else {
        log_ = req.output_csv;
        log_.replace_extension(".log");
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE logh = CreateFileW(log_.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW si{sizeof si};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = logh;
        si.hStdError = logh;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args.str();
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (logh != INVALID_HANDLE_VALUE) CloseHandle(logh);
        if (!ok) {
            if (error) *error = "PresentMon could not be started (error " + std::to_string(GetLastError()) + ").";
            return false;
        }
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
    }
    started_us_ = qpc_now_us();
    return true;
}

CaptureStatus PresentMonRunner::poll() {
    CaptureStatus s;
    if (!process_) return s;
    s.elapsed_s = double(qpc_now_us() - started_us_) / 1e6;
    const fs::path dir = req_.output_csv.parent_path();
    const std::string stem = narrow(req_.output_csv.stem().wstring());
    const fs::path csv = newest_matching(dir, stem, ".csv");
    if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
        if (csv.empty()) {
            s.state = CaptureState::Waiting;
            s.message = !req_.hotkey.empty()
                            ? "Switch to VALORANT, get into position and press " + req_.hotkey + " to start a " +
                                  std::to_string(req_.duration_s) + " s recording."
                            : "Recording starts after the countdown. Switch to VALORANT now.";
        } else {
            s.state = CaptureState::Recording;
            s.message = "Recording " + std::to_string(req_.duration_s) + " s… keep playing the same scenario.";
        }
        return s;
    }
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    CloseHandle(process_);
    process_ = nullptr;
    std::error_code ec;
    if (!csv.empty() && fs::file_size(csv, ec) > 200) {
        s.state = CaptureState::Finished;
        s.csv = csv;
        s.message = "Capture complete.";
        return s;
    }
    s.state = CaptureState::Failed;
    std::string log;
    if (!log_.empty()) {
        std::string err;
        if (auto t = fit::read_file(log_, &err, 1 << 20)) log = *t;
    }
    std::string lower = log;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("access denied") != std::string::npos || lower.find("elevat") != std::string::npos)
        s.message = "PresentMon was not allowed to start a trace session. Tick \"Run PresentMon as administrator\" or add "
                    "your account to the \"Performance Log Users\" group, then sign out and back in.";
    else
        s.message = "PresentMon ended without writing frame data (exit code " + std::to_string(code) +
                    "). Was VALORANT running and presenting frames?" + (log.empty() ? "" : "\n" + log.substr(0, 600));
    return s;
}

void PresentMonRunner::cancel() {
    if (!process_) return;
    TerminateProcess(process_, 1);
    CloseHandle(process_);
    process_ = nullptr;
}

}  // namespace fitapp::win32
