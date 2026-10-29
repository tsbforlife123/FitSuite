// Settings: a short list of plain options, like the options dialog of any Windows utility.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "app.hpp"
#include "fit/game_activation.hpp"
#include "fit/mouse_db.hpp"
#include "fit/timeutil.hpp"
#include "fit/update.hpp"
#include "fit/windows_checks.hpp"
#include "imgui.h"
#include "theme.hpp"

namespace fitapp {

using namespace fit;

namespace {

constexpr float kLabelW = 190.0f;

bool begin_rows(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, kLabelW);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
}

void appearance(App& app, float w) {
    auto& st = app.setup().app;
    ui::begin_card("##set_look", w, 0);
    ui::card_title("Appearance");
    if (begin_rows("##look")) {
        row("Theme");
        int t = st.theme == "light" ? 1 : st.theme == "dark" ? 2 : 0;
        bool changed = ImGui::RadioButton("Same as Windows", &t, 0);
        ImGui::SameLine(0, 18);
        changed |= ImGui::RadioButton("Light", &t, 1);
        ImGui::SameLine(0, 18);
        changed |= ImGui::RadioButton("Dark", &t, 2);
        if (changed) {
            st.theme = t == 1 ? "light" : t == 2 ? "dark" : "system";
            app.save();
        }
        ImGui::EndTable();
    }
    if (st.theme == "system") ui::faint("Follows Windows: Settings → Personalization → Colors → app mode.");
    ui::end_card();
}

void mouse(App& app, float w) {
    auto& setup = app.setup();
    if (!app.model_input_loaded) {
        std::snprintf(app.model_input, sizeof app.model_input, "%s", setup.mouse.model.c_str());
        app.model_input_loaded = true;
    }
    ui::begin_card("##set_mouse", w, 0);
    ui::card_title("Your mouse");
    if (begin_rows("##mouse")) {
        row("Mouse model");
        mouse_model_field(app, std::min(320.0f, w - kLabelW - 40.0f));
        if (!setup.mouse.name.empty() || !setup.mouse.vid_pid.empty()) {
            row("");
            ui::faint("Detected: %s%s%s", setup.mouse.name.c_str(), setup.mouse.vid_pid.empty() ? "" : "  ",
                      setup.mouse.vid_pid.c_str());
        }
        row("DPI");
        int dpi = setup.mouse.dpi;
        ImGui::SetNextItemWidth(110);
        if (ImGui::InputInt("##dpi", &dpi, 0, 0)) {
            setup.mouse.dpi = std::clamp(dpi, 50, 64000);
            app.save();
        }
        row("Main game");
        main_game_picker(app);
        row(main_game_sens_label(setup).c_str());
        main_game_sens_input(app, 110);
        row("");
        ui::faint("%s", game_sens_where(main_game_exe(setup.game.id)).c_str());
        row("Full turn (360°)");
        const double cm = main_game_cm360(setup);
        if (cm > 0) ImGui::Text("%.1f cm of mouse movement", cm);
        else ImGui::TextDisabled("enter your setting");
        row("Check reminder");
        int r = setup.app.check_reminder_days >= 30 ? 2 : setup.app.check_reminder_days > 0 ? 1 : 0;
        bool changed = ImGui::RadioButton("Every week", &r, 1);
        ImGui::SameLine(0, 18);
        changed |= ImGui::RadioButton("Every month", &r, 2);
        ImGui::SameLine(0, 18);
        changed |= ImGui::RadioButton("Never", &r, 0);
        if (changed) {
            setup.app.check_reminder_days = r == 1 ? 7 : r == 2 ? 30 : 0;
            app.save();
        }
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, 2));
    if (ui::button_secondary("Measure again (about 4 min)")) app.start_calibration();
    ImGui::SameLine();
    MouseProfileRecord* fix = app.active_fix();
    ImGui::BeginDisabled(!fix);
    if (ui::button_secondary("Quick check (about 1 min)")) app.start_check();
    ImGui::EndDisabled();
    if (fix) {
        ImGui::SameLine(0, 16);
        if (ui::confirm_link("Stop using my fix", "Stop using it? Your own Raw Accel settings come back.")) app.remove_fix();
    }
    // The quick check: about a minute with the fix in use, to see whether it still fits.
    if (fix && fix->last_check) {
        const auto& lc = *fix->last_check;
        const unsigned c = lc.verdict == "changed" ? theme::kWarn : lc.verdict == "still_fits" ? theme::kGood : theme::kTextMuted;
        ui::wrapped(lc.explanation.c_str(), c);
        ui::faint("Last quick check %s", short_time(lc.finished_utc).c_str());
    } else if (fix) {
        ui::faint("The quick check tells you, in about a minute, whether your fix still fits or your aim has changed.");
    }
    ui::faint("A new mouse, DPI or sensitivity changes how your flicks land: measure again after changing any of them.");
    ui::end_card();
}

void startup(App& app, float w) {
    auto& setup = app.setup();
    auto& st = setup.app;
    ui::begin_card("##set_start", w, 0);
    ui::card_title("Startup and background");
    bool sw = st.start_with_windows;
    if (ImGui::Checkbox("Open FitSuite when Windows starts", &sw)) app.set_start_with_windows(sw);
    if (app.startup_blocked())
        ui::wrapped("Windows has FitSuite switched off under Task Manager → Startup apps, so it will not start. Untick and "
                    "tick this box to switch it back on.",
                    theme::kWarn);
    ImGui::Indent(28);
    ImGui::BeginDisabled(!st.start_with_windows);
    bool hidden = st.start_hidden;
    if (ImGui::Checkbox("Start in the notification area, without opening the window", &hidden)) {
        st.start_hidden = hidden;
        app.save();
    }
    ImGui::EndDisabled();
    ImGui::Unindent(28);
    const bool needed = setup.fix_games.enabled;
    bool tray = st.close_to_tray || needed;
    ImGui::BeginDisabled(needed);
    if (ImGui::Checkbox("Keep running in the notification area when I close the window", &tray)) {
        st.close_to_tray = tray;
        app.save();
    }
    ImGui::EndDisabled();
    if (needed) ui::faint("Always on while your fix switches per game: FitSuite has to run for that.");
    ui::faint("In the background FitSuite draws nothing, holds no graphics memory and runs in Windows' efficiency mode. It "
              "wakes only when there is work: a program switch while your fix switches per game, or the reminder check "
              "once a minute. Right-click its icon in the notification area to open or quit it.");
    ui::end_card();
}

void tools(App& app, float w) {
    auto& st = app.setup().app;
    ui::begin_card("##set_tools", w, 0);
    ui::card_title("Extra tools");
    bool extras = st.show_extra_tools;
    if (ImGui::Checkbox("Show the Frame rate and Monitor tabs", &extras)) {
        st.show_extra_tools = extras;
        app.save();
    }
    ui::faint("Frame rate measures how smoothly VALORANT runs (with Intel PresentMon); Monitor compares refresh rates and "
              "overdrive by eye. Neither is needed for the mouse fix.");
    ui::end_card();
}

void game_sensitivity(App& app, float w) {
    auto& setup = app.setup();
    ui::begin_card("##set_games", w, 0);
    const std::string main_name = main_game_name(setup);
    const std::string intro = "Your fix is measured at your " + main_name +
                              " setting. It fits other games best where a full turn takes about the same mouse distance.";
    ui::card_title("Game sensitivity", intro.c_str());
    const double val_cm = main_game_cm360(setup);
    std::vector<GameChoice> rows;
    for (const auto& c : game_choices(setup.fix_games.games)) {
        bool on = false;
        for (const auto& e : c.exes)
            on = on || std::find(setup.fix_games.games.begin(), setup.fix_games.games.end(), e) != setup.fix_games.games.end();
        if (on) rows.push_back(c);
    }
    if (rows.empty()) {
        ui::faint("Tick your games on the Mouse tab (\"2  Use it in your games\") to compare them here.");
        ui::end_card();
        return;
    }
    const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##gs", 4, tf)) {
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Your setting", ImGuiTableColumnFlags_WidthFixed, 230);
        ImGui::TableSetupColumn("Full turn", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(("Compared with " + main_name).c_str(), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& c : rows) {
            ImGui::PushID(c.exes.front().c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(c.name.c_str());
            ImGui::TableNextColumn();
            const bool valorant = is_main_game(setup, c);  // the main game (entered under Your mouse)
            if (valorant) {
                ImGui::AlignTextToFramePadding();
                if (c.yaw > 0.0) ui::muted("%.3f", setup.game.sensitivity);
                else ui::muted("%.1f cm per 360°", setup.game.cm360);
                ImGui::SameLine();
                ui::faint("(under Your mouse)");
            } else {
                GameSensitivity& gs = setup.app.game_sens[c.exes.front()];
                ImGui::SetNextItemWidth(90);
                if (c.yaw > 0.0) {
                    float v = float(gs.sensitivity);
                    if (ImGui::InputFloat("##v", &v, 0.0f, 0.0f, v > 0 ? "%.3f" : "")) {
                        gs.sensitivity = std::clamp(double(v), 0.0, 10000.0);
                        app.save();
                    }
                    ImGui::SameLine();
                    ui::faint("in-game");
                } else {
                    float v = float(gs.cm360);
                    if (ImGui::InputFloat("##v", &v, 0.0f, 0.0f, v > 0 ? "%.1f" : "")) {
                        gs.cm360 = std::clamp(double(v), 0.0, 10000.0);
                        app.save();
                    }
                    ImGui::SameLine();
                    ui::faint("cm per 360°");
                    ui::help_marker("This game's sensitivity scale depends on its field of view, so FitSuite cannot convert "
                                    "it. Measure how far you move the mouse for one full turn in the game, or use a "
                                    "sensitivity converter, and enter the distance here.");
                }
            }
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            const double cm = game_cm360(setup, c);
            if (cm > 0.0) ImGui::Text("%.1f cm", cm);
            else ImGui::TextDisabled("—");
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (valorant) {
                ui::muted("fix measured here");
            } else {
                bool warn = false;
                const std::string cmp = turn_comparison(cm, val_cm, main_name, &warn, true);
                if (cmp.empty()) ImGui::TextDisabled("enter your setting");
                else ui::wrapped(cmp.c_str(), warn ? theme::kWarn : theme::kGood);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ui::faint("Conversions use each game's published turn rate (VALORANT 0.07°, Counter-Strike 2 and Apex 0.022°, "
              "Overwatch 2 and Call of Duty 0.0066° per count at sensitivity 1).");
    ui::end_card();
}

// Read only: what a few game-related Windows settings are set to, the usual choice, and a link to
// the Windows page that changes each one. Read again every few seconds while Settings is open, so a
// change made in Windows shows up here.
void windows_settings(App& app, float w) {
    static double read_at = -100.0;
    static std::vector<fit::WindowsCheck> checks;
    if (ImGui::GetTime() - read_at > 3.0) {
        read_at = ImGui::GetTime();
        checks = fit::windows_checks(app.platform().windows_game_settings());
    }
    ui::begin_card("##set_windows", w, 0);
    ui::card_title("Windows settings for games",
                   "FitSuite only reads these. Click Open to change one in Windows; the usual choice is in grey.");
    const ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##winchecks", 3, tf)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 200);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("open", ImGuiTableColumnFlags_WidthFixed, 50);
        for (const auto& c : checks) {
            ImGui::PushID(c.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(c.name.c_str());
            ImGui::TableNextColumn();
            const unsigned col = c.state == fit::WindowsCheck::State::Look      ? theme::kWarn
                                 : c.state == fit::WindowsCheck::State::Unknown ? theme::kTextMuted
                                                                                : theme::kGood;
            ImGui::TextColored(theme::v4(col), "%s", c.current.c_str());
            ui::faint("Usual: %s", c.usual.c_str());
            ui::wrapped(c.note.c_str(), theme::kTextMuted);
            ImGui::TableNextColumn();
            if (ui::button_link("Open")) app.platform().open_url(c.settings_uri);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ui::end_card();
}

void check_setup(App& app, float w) {
    ui::begin_card("##set_check", w, 0);
    ui::card_title("Check my setup", "Looks at everything FitSuite needs on this PC and says what is missing.");
    if (ui::button_primary("Check now")) app.diagnostics = app.check_setup();
    if (!app.diagnostics.empty()) {
        ImGui::SameLine();
        ui::faint("Checked %s", short_time(app.diagnostics_time).c_str());
        const ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("##diag", 3, tf)) {
            ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthFixed, 190);
            ImGui::TableSetupColumn("detail", ImGuiTableColumnFlags_WidthStretch);
            for (const auto& d : app.diagnostics) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(theme::v4(d.ok ? theme::kGood : theme::kWarn), "%s", d.ok ? "OK" : "Check");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(d.what.c_str());
                ImGui::TableNextColumn();
                ui::wrapped(d.detail.c_str(), theme::kTextMuted);
            }
            ImGui::EndTable();
        }
    }
    ui::end_card();
}

void updates(App& app, float w) {
    auto& st = app.setup().app;
    auto& u = app.updater;
    ui::begin_card("##set_updates", w, 0);
    ui::card_title("Updates");
    bool up = st.update_check;
    if (ImGui::Checkbox("Check for updates automatically", &up)) {
        st.update_check = up;
        app.save();
    }
    ui::faint("On by default: each time FitSuite opens, and once a day while it runs. From FitSuite's public releases "
              "(github.com/%s). Nothing about you is sent.", fit::kUpdateRepo);
    ImGui::Dummy(ImVec2(0, 2));
    using S = Updater::Step;
    switch (u.step) {
        case S::Idle:
        case S::UpToDate:
        case S::Failed: {
            ImGui::BeginDisabled(app.update_busy());
            if (ui::button_secondary("Check now")) app.update_check(true);
            ImGui::EndDisabled();
            ImGui::SameLine(0, 12);
            ImGui::AlignTextToFramePadding();
            if (u.step == S::UpToDate) ImGui::TextColored(theme::v4(theme::kGood), "You have the latest version (%s).", kAppVersion);
            else if (u.step == S::Failed) ui::wrapped(u.message.c_str(), theme::kWarn);
            else if (!st.last_update_check_utc.empty()) ui::faint("Last checked %s", short_time(st.last_update_check_utc).c_str());
            break;
        }
        case S::Checking: ui::muted("Checking…"); break;
        case S::Available: {
            ImGui::TextColored(theme::v4(theme::kAccent), "Version %s is available (you have %s).", u.release->version_text().c_str(), kAppVersion);
            if (!u.release->notes.empty()) {
                std::string notes = u.release->notes;
                if (notes.size() > 600) notes = notes.substr(0, 600) + "…";
                ui::wrapped(notes.c_str(), theme::kTextMuted);
            }
            if (ui::button_primary("Download the update")) app.update_download();
            break;
        }
        case S::Downloading: {
            const float f = u.total ? float(double(u.bytes) / double(u.total)) : 0.0f;
            ui::progress_bar(f, ui::fmt("Downloading version %s… %.1f MB", u.release ? u.release->version_text().c_str() : "", double(u.bytes) / 1048576.0).c_str(),
                             360, 14);
            break;
        }
        case S::Ready: {
            ImGui::TextColored(theme::v4(theme::kGood), "Version %s downloaded and checked.", u.release ? u.release->version_text().c_str() : "");
            if (ui::button_primary("Update now (FitSuite restarts)")) app.update_install();
            ImGui::SameLine(0, 12);
            ui::faint("Your settings and measurements stay as they are.");
            break;
        }
    }
    ui::end_card();
}

void about(App& app, float w) {
    ui::begin_card("##set_about", w, 0);
    ui::card_title("About");
    ImGui::TextUnformatted("FitSuite");
    ui::faint("Local only: no account, no cloud, nothing uploaded. Not tested on Windows hardware yet; please report anything "
              "that does not work as described.");
    ui::faint("Your data: %s", app.platform().data_dir().string().c_str());
    if (ui::button_secondary("Open data folder")) app.platform().open_folder(app.platform().data_dir());
    ImGui::SameLine(0, 16);
    if (ui::button_link("Run the setup again")) {
        app.setup().app.setup_done = false;
        app.setup().app.setup_step = 0;
        app.save();
    }
    ui::end_card();
}

}  // namespace

void mouse_model_field(App& app, float width) {
    auto& setup = app.setup();
    if (!app.model_input_loaded) {
        std::snprintf(app.model_input, sizeof app.model_input, "%s", setup.mouse.model.c_str());
        app.model_input_loaded = true;
    }
    auto set_model = [&](const std::string& m) {
        setup.mouse.model = m;
        std::snprintf(app.model_input, sizeof app.model_input, "%s", m.c_str());
        app.save();
    };
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputTextWithHint("##model", "e.g. Logitech G Pro X Superlight 2", app.model_input, sizeof app.model_input)) {
        std::string m = app.model_input;
        while (!m.empty() && m.back() == ' ') m.pop_back();
        setup.mouse.model = m;
        app.save();
    }
    const KnownMouse* known = find_mouse(setup.mouse.model);
    if (!known) {
        // Matches from the list, as plain lines to click (no pop-up).
        const auto matches = search_mice(app.model_input);
        for (const KnownMouse* m : matches) {
            ImGui::PushID(m);
            if (ImGui::Selectable(m->full_name().c_str(), false, 0, ImVec2(width, 0))) set_model(m->full_name());
            ImGui::PopID();
        }
        if (matches.empty() && std::strlen(app.model_input) >= 3)
            ui::faint("Not in FitSuite's list of %d mice: that is fine, the test works with any mouse.", int(known_mice().size()));
        return;
    }
    // Published specs of the chosen mouse, and anything that does not fit them.
    std::string specs;
    auto add = [&](const std::string& s) { specs += (specs.empty() ? "" : " · ") + s; };
    if (!known->sensor.empty()) add(known->sensor + " sensor");
    if (!known->dpi_steps.empty()) {
        std::string st;
        for (int s : known->dpi_steps) st += (st.empty() ? "" : "/") + std::to_string(s);
        add(st + " DPI");
    } else if (known->max_dpi > 0) {
        add(known->min_dpi > 0 ? ui::fmt("%d–%d DPI", known->min_dpi, known->max_dpi) : ui::fmt("up to %d DPI", known->max_dpi));
    }
    if (known->max_polling_hz > 0) add(ui::fmt("up to %d Hz", known->max_polling_hz));
    ui::faint("%s", specs.c_str());
    if (!known->polling_note.empty()) ui::faint("%s", known->polling_note.c_str());
    ui::faint("Specs from %s (checked %s).", known->source.c_str(), known->checked.c_str());
    const MouseAdvice adv = mouse_advice(*known, setup.mouse.dpi, setup.mouse.polling_hz);
    for (const auto& w : adv.warnings) ui::wrapped(w.c_str(), theme::kWarn);
    for (const auto& n : adv.notes) ui::faint("%s", n.c_str());
}

void page_settings(App& app) {
    ui::page_header("Settings", "How FitSuite looks, your mouse, and how it runs in the background.");
    const float avail = ImGui::GetContentRegionAvail().x;
    const bool two_cols = avail >= 1100.0f;
    const float gap = 14.0f;
    const float w = two_cols ? std::floor((avail - gap) * 0.5f) : std::min(avail, 760.0f);
    ImGui::BeginGroup();
    appearance(app, w);
    mouse(app, w);
    startup(app, w);
    tools(app, w);
    ImGui::EndGroup();
    if (two_cols) ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    game_sensitivity(app, w);
    check_setup(app, w);
    windows_settings(app, w);
    updates(app, w);
    about(app, w);
    ImGui::EndGroup();
}

}  // namespace fitapp
