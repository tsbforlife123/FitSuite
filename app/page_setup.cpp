// First-run setup: a short guided list, like an installer. Shown instead of the tabs until it is
// finished or skipped; Settings → About can start it again.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "app.hpp"
#include "fit/game_activation.hpp"
#include "fit/update.hpp"
#include "imgui.h"
#include "theme.hpp"

namespace fitapp {

using namespace fit;
namespace fs = std::filesystem;

namespace {

constexpr int kSteps = 6;
const char* kStepNames[kSteps] = {"Welcome", "Your mouse", "Your monitor", "Raw Accel", "Startup and updates", "Ready"};

void check_line(bool ok, const char* text) {
    ImGui::TextColored(theme::v4(ok ? theme::kGood : theme::kTextFaint), "%s", ok ? "OK" : "—");
    ImGui::SameLine(0, 10);
    ImGui::TextUnformatted(text);
}

void step_welcome(App&) {
    ui::wrapped("FitSuite measures how your flicks land in each direction and fixes the directions where you "
                "undershoot or overshoot, through the free Raw Accel driver, only while your games are in front.");
    ImGui::Dummy(ImVec2(0, 4));
    ui::wrapped("This setup takes about two minutes:", theme::kTextMuted);
    ui::wrapped("  1. Your mouse: DPI and VALORANT sensitivity.", theme::kTextMuted);
    ui::wrapped("  2. Your monitor: make sure it runs at full speed.", theme::kTextMuted);
    ui::wrapped("  3. Raw Accel: install it, or find the one you have.", theme::kTextMuted);
    ui::wrapped("  4. Startup and updates.", theme::kTextMuted);
    ImGui::Dummy(ImVec2(0, 4));
    ui::faint("Everything stays on this PC: no account, nothing uploaded. FitSuite never reads the screen or a game, and "
              "never moves your mouse.");
}

bool step_mouse(App& app) {
    auto& setup = app.setup();
    if (!app.model_input_loaded) {
        std::snprintf(app.model_input, sizeof app.model_input, "%s", setup.mouse.model.c_str());
        app.model_input_loaded = true;
    }
    if (ImGui::BeginTable("##sm", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, 190);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        auto row = [](const char* label) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
        };
        row("Mouse model");
        ImGui::SetNextItemWidth(320);
        if (ImGui::InputTextWithHint("##model", "e.g. Logitech G Pro X Superlight 2", app.model_input, sizeof app.model_input)) {
            std::string m = app.model_input;
            while (!m.empty() && m.back() == ' ') m.pop_back();
            setup.mouse.model = m;
            app.save();
        }
        if (!setup.mouse.name.empty()) {
            row("");
            ui::faint("Detected: %s", setup.mouse.name.c_str());
        }
        row("DPI");
        int dpi = setup.mouse.dpi;
        ImGui::SetNextItemWidth(110);
        if (ImGui::InputInt("##dpi", &dpi, 0, 0)) {
            setup.mouse.dpi = std::clamp(dpi, 50, 64000);
            app.save();
        }
        ImGui::SameLine();
        ui::faint("in your mouse's own software (G HUB, Synapse, …), or on the mouse's DPI button");
        row("VALORANT sensitivity");
        float sens = float(setup.game.sensitivity);
        ImGui::SetNextItemWidth(110);
        if (ImGui::InputFloat("##sens", &sens, 0.0f, 0.0f, "%.3f")) {
            setup.game.sensitivity = std::clamp(double(sens), 0.001, 20.0);
            app.save();
        }
        ImGui::SameLine();
        ui::faint("VALORANT: Settings → General → Mouse → Sensitivity: Aim");
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, 4));
    if (setup.mouse.dpi > 0 && setup.game.sensitivity > 0)
        ui::faint("A full turn (360°) takes %.1f cm of mouse movement.", valorant_cm360(setup));
    ui::faint("800 and 0.400 are only examples: enter your own, so the test feels exactly like your game.");
    return setup.mouse.dpi > 0 && setup.game.sensitivity > 0;
}

void step_monitor(App& app) {
    const auto slow = app.slow_displays();
    if (slow.empty()) {
        for (const auto& d : app.displays()) {
            const auto a = fit::refresh_advice(d.refresh_hz, d.modes_hz);
            const std::string name = d.name.empty() ? d.device : d.name;
            check_line(d.refresh_hz > 0, ui::fmt("%s runs at %.0f Hz%s.", name.c_str(), d.refresh_hz,
                                                 a.max_hz > 0 && !a.slower ? ", the fastest it offers" : "")
                                             .c_str());
        }
        if (app.displays().empty()) ui::faint("No monitor information from Windows.");
        ImGui::Dummy(ImVec2(0, 4));
        ui::faint("If your monitor is sold as faster than this, Windows may not offer it over this cable or port: use "
                  "DisplayPort, or the cable that came with the monitor.");
    } else {
        refresh_rate_notice(app);
    }
}

void step_rawaccel(App& app) {
    auto& r = app.ra_install;
    const bool driver = app.rawaccel_installed();
    const fs::path folder = app.rawaccel_folder();
    std::error_code ec;
    const bool has_settings = !folder.empty() && fs::is_regular_file(folder / "settings.json", ec);
    ui::wrapped("Raw Accel is a free, signed, open-source mouse driver (MIT licence). FitSuite uses it to apply your fix "
                "in games; its FAQ says it has been working in VALORANT matches (FitSuite has not verified that). You can "
                "measure your aim and run the blind test without it.",
                theme::kTextMuted);
    ImGui::Dummy(ImVec2(0, 4));

    if (driver && has_settings) {
        check_line(true, "Raw Accel is installed.");
        check_line(true, ("Its settings: " + (folder / "settings.json").string()).c_str());
        return;
    }
    if (driver) {
        check_line(true, "The Raw Accel driver is installed.");
        if (!folder.empty()) {
            check_line(false, "Raw Accel has not saved its settings yet.");
            ui::wrapped("Open Raw Accel once and click Apply (the defaults are fine), so it writes its settings file. "
                        "FitSuite notices it here.");
            if (ui::button_primary("Open Raw Accel")) app.open_rawaccel_gui();
        } else {
            check_line(false, "FitSuite could not find Raw Accel's folder.");
            if (ui::button_secondary("Find settings.json…")) {
                if (auto p = app.platform().open_file_dialog("Raw Accel settings.json", "Raw Accel settings", "settings.json"))
                    app.remember_rawaccel_settings_path(*p);
            }
        }
        return;
    }
    if (app.setup().app.rawaccel_install_pending && r.step == RaInstall::Step::Idle) {
        ui::callout(ui::Tone::Info, "Restart your PC to finish installing Raw Accel",
                    "Raw Accel's installer has run; the driver starts after a restart. Open FitSuite again afterwards and "
                    "the setup continues here.");
        if (ui::confirm_link("Restart now", "Restart now? Save your work in other programs first.")) {
            std::string err;
            if (!app.platform().restart_pc(&err)) app.toast(ui::Tone::Bad, err, 8.0);
        }
        ImGui::SameLine(0, 16);
        if (ui::button_link("Install again")) app.ra_install_start();
        return;
    }
    if (app.rawaccel_needs_vc_runtime()) {
        ui::callout(ui::Tone::Warn, "Raw Accel needs the Microsoft Visual C++ runtime",
                    "It is not installed on this PC. Install it from Microsoft first (one click, about 25 MB).");
        if (ui::button_secondary("Get it from Microsoft")) app.platform().open_url(kVcRuntimeUrl);
        ImGui::Dummy(ImVec2(0, 4));
    }
    switch (r.step) {
        case RaInstall::Step::Idle:
        case RaInstall::Step::Failed: {
            if (r.step == RaInstall::Step::Failed) ui::callout(ui::Tone::Bad, "Raw Accel was not installed", r.message.c_str());
            ui::wrapped(ui::fmt("FitSuite downloads the official Raw Accel %s release from GitHub, checks that it is exactly "
                                "that release, unpacks it to %s and starts its installer. Windows asks for your permission "
                                "(it installs a driver), and a restart finishes it.",
                                kRawAccelVersion, (app.platform().programs_dir() / "RawAccel").string().c_str())
                            .c_str());
            if (ui::button_primary("Download and install Raw Accel")) app.ra_install_start();
            ImGui::SameLine(0, 16);
            if (ui::button_link("Download it myself")) app.platform().open_url(kRawAccelReleasesPage);
            break;
        }
        case RaInstall::Step::Downloading: {
            const float f = r.total ? float(double(r.bytes) / double(r.total)) : 0.0f;
            ui::progress_bar(f, ui::fmt("Downloading Raw Accel… %.1f MB", double(r.bytes) / 1048576.0).c_str(), 360, 14);
            break;
        }
        case RaInstall::Step::Unzipping: ui::muted("Unpacking…"); break;
        case RaInstall::Step::Installing:
            ui::muted("Raw Accel's installer is running. Allow it at Windows' prompt and follow its window.");
            break;
        case RaInstall::Step::NeedsRestart:
            ui::callout(ui::Tone::Good, "Installed. Restart your PC to start the driver",
                        "Open FitSuite again after the restart: the setup continues here, and FitSuite helps you open Raw "
                        "Accel once so it saves its settings.");
            if (ui::confirm_link("Restart now", "Restart now? Save your work in other programs first.")) {
                std::string err;
                if (!app.platform().restart_pc(&err)) app.toast(ui::Tone::Bad, err, 8.0);
            }
            break;
    }
}

void step_startup(App& app) {
    auto& st = app.setup().app;
    bool sw = st.start_with_windows;
    if (ImGui::Checkbox("Open FitSuite when Windows starts (it waits in the notification area)", &sw)) app.set_start_with_windows(sw);
    ui::faint("Needed for your fix to switch on in your games after a restart. In the background FitSuite uses next to "
              "nothing.");
    ImGui::Dummy(ImVec2(0, 6));
    bool up = st.update_check;
    if (ImGui::Checkbox("Check for updates when FitSuite opens", &up)) {
        st.update_check = up;
        app.save();
    }
    ui::faint("FitSuite asks GitHub (github.com/%s) for the latest release. Nothing about you is sent. Updates install only "
              "when you click Update, and only if they match their published checksum.",
              kUpdateRepo);
}

void step_ready(App& app) {
    auto& setup = app.setup();
    std::error_code ec;
    const fs::path folder = app.rawaccel_folder();
    check_line(setup.mouse.dpi > 0 && setup.game.sensitivity > 0,
               ui::fmt("Mouse: %d DPI, VALORANT sensitivity %.3f", setup.mouse.dpi, setup.game.sensitivity).c_str());
    check_line(app.slow_displays().empty(), app.slow_displays().empty() ? "Monitor runs at full speed"
                                                                         : "Monitor below its fastest refresh rate");
    check_line(app.rawaccel_installed() && !folder.empty() && fs::is_regular_file(folder / "settings.json", ec),
               app.rawaccel_installed() ? "Raw Accel installed" : "Raw Accel not installed yet (needed only to use your fix in games)");
    check_line(setup.app.start_with_windows, setup.app.start_with_windows ? "Opens when Windows starts" : "Does not open when Windows starts");
    ImGui::Dummy(ImVec2(0, 6));
    ui::wrapped("Next: measure your flicks. Five short rounds, about 4 minutes: flicks, micro-adjustments, tracking and "
                "target switching. Use the mouse, pad and grip you play with.");
}

}  // namespace

void page_setup(App& app) {
    auto& st = app.setup().app;
    int& step = st.setup_step;
    step = std::clamp(step, 0, kSteps - 1);
    ui::push_font(theme::fonts().semibold, theme::kH2);
    ImGui::TextUnformatted("Set up FitSuite");
    ui::pop_font();
    ui::page_header("Set up FitSuite", "A few questions, then you are ready to measure your aim.");

    // Step list on the left, like an installer.
    const float avail = ImGui::GetContentRegionAvail().x;
    ui::begin_card("##setup_steps", 230, 0);
    for (int i = 0; i < kSteps; ++i) {
        const bool cur = i == step, done = i < step;
        ImGui::TextColored(theme::v4(cur ? theme::kAccent : done ? theme::kGood : theme::kTextFaint), "%s", done ? "OK" : cur ? ">" : " ");
        ImGui::SameLine(0, 10);
        if (cur) ImGui::TextUnformatted(kStepNames[i]);
        else ui::muted("%s", kStepNames[i]);
    }
    ui::end_card();
    ImGui::SameLine(0, 14);

    ui::begin_card("##setup_body", std::min(avail - 244.0f, 860.0f), 0);
    ui::card_title(kStepNames[step]);
    bool can_next = true;
    switch (step) {
        case 0: step_welcome(app); break;
        case 1: can_next = step_mouse(app); break;
        case 2: step_monitor(app); break;
        case 3: step_rawaccel(app); break;
        case 4: step_startup(app); break;
        case 5: step_ready(app); break;
    }
    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));
    if (step > 0) {
        if (ui::button_secondary("Back")) {
            --step;
            app.save();
        }
        ImGui::SameLine();
    }
    if (step == 0) {
        if (ui::button_primary("Start the setup")) {
            step = 1;
            app.save();
        }
    } else if (step < kSteps - 1) {
        const bool busy = step == 3 && app.ra_install.token != 0;
        ImGui::BeginDisabled(!can_next || busy);
        if (ui::button_primary(step == 3 && !app.rawaccel_installed() ? "Next (set up Raw Accel later)" : "Next")) {
            ++step;
            app.save();
        }
        ImGui::EndDisabled();
        if (!can_next) {
            ImGui::SameLine();
            ui::faint("Enter your DPI and sensitivity first.");
        }
    } else {
        if (ui::button_primary("Measure my flicks")) {
            app.finish_setup();
            app.go(Page::MouseFit);
            app.start_calibration();
        }
        ImGui::SameLine();
        if (ui::button_secondary("Go to FitSuite")) {
            app.finish_setup();
            app.go(Page::MouseFit);
        }
    }
    if (step < kSteps - 1) {
        ImGui::SameLine(0, 24);
        if (ui::button_link("Skip the setup")) {
            app.finish_setup();
            app.go(Page::MouseFit);
        }
    }
    ui::end_card();
}

}  // namespace fitapp
