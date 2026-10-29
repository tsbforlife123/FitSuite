#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "app.hpp"
#include "fit/serialize.hpp"
#include "fit/store.hpp"
#include "fit/timeutil.hpp"
#include "imgui.h"

namespace fitapp {

using namespace fit;
using theme::fonts;

namespace {

std::string describe_setting(const std::string& k) {
    if (k == "mouse.dpi") return "Mouse DPI";
    if (k == "game.sensitivity") return "In-game sensitivity";
    if (k == "game.scoped_multiplier") return "VALORANT scoped multiplier";
    if (k == "game.fps_cap") return "VALORANT FPS limit";
    if (k == "display.refresh_hz") return "Display refresh rate";
    if (k == "display.resolution") return "Display resolution";
    if (k == "display.overdrive") return "Monitor overdrive";
    if (k == "mousefit.active_profile") return "MouseFit correction";
    if (k.rfind("game.graphics.", 0) == 0) return "VALORANT " + k.substr(14);
    return k;
}

// How a setting gets back to its baseline value: FitSuite can do it, or the player has to.
const char* restore_how(const std::string& k) {
    if (k == "mousefit.active_profile") return "FitSuite restores this";
    if (k == "display.refresh_hz") return "FitSuite can restore this";
    if (k == "mouse.dpi") return "Change it in your mouse software";
    if (k.rfind("game.", 0) == 0) return "Change it in the game";
    if (k == "display.overdrive") return "Change it in the monitor menu";
    return "Change it manually";
}

}  // namespace

void page_profiles(App& app) {
    auto& setup = app.setup();
    ui::page_header("Saved Profiles", "Your setup profile, mouse profiles, baseline and every change FitSuite made — with "
                                      "the way back. Files are versioned, validated on load and saved atomically.");
    const float avail = ImGui::GetContentRegionAvail().x;
    const float w = (avail - 14) * 0.5f;

    ui::begin_card("##pr_setup", w, 0);
    ui::card_title("Setup profile", nullptr);
    char name[128];
    std::snprintf(name, sizeof name, "%s", setup.name.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##setupname", name, sizeof name)) setup.name = name;
    if (ImGui::IsItemDeactivatedAfterEdit()) app.save();
    ui::kv("Created", short_time(setup.created_utc));
    ui::kv("Last saved", short_time(setup.updated_utc));
    ui::kv("Format", ui::fmt("fitsuite.setup v%d", setup.schema_version));
    ui::kv("Data folder", app.store().root().string());
    ui::kv("Backups", ui::fmt("%zu kept", app.store().list_backups().size()));
    if (ui::button_secondary("Open data folder")) app.platform().open_folder(app.store().root());
    ImGui::SameLine();
    if (ui::button_secondary("Export setup…")) {
        if (auto p = app.platform().save_file_dialog("Export setup", "fitsuite-setup.json", "FitSuite setup", "*.json")) {
            std::string err;
            if (atomic_write_file(*p, to_json(setup).dump(2), &err)) app.toast(ui::Tone::Good, "Exported to " + p->string());
            else app.toast(ui::Tone::Bad, err);
        }
    }
    ui::end_card();

    ImGui::SameLine(0, 14);
    ui::begin_card("##pr_base", w, 0);
    ui::card_title("Baseline and restore", "Settings recorded as your baseline compared with now.");
    if (setup.baseline.empty()) {
        ui::muted("No baseline recorded.");
        if (ui::button_primary("Record baseline now")) app.capture_baseline();
    } else {
        ui::muted("Recorded %s", short_time(setup.baseline_captured_utc).c_str());
        const auto now = snapshot_settings(setup);
        int diffs = 0;
        if (ImGui::BeginTable("##base", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            for (const char* h : {"Setting", "Baseline", "Now", "How to restore"}) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            for (const auto& [k, v] : setup.baseline) {
                auto it = now.find(k);
                const std::string cur = it == now.end() ? "—" : it->second;
                const bool diff = cur != v;
                diffs += diff ? 1 : 0;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(describe_setting(k).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(v.c_str());
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, theme::v4(diff ? theme::kWarn : theme::kText));
                ImGui::TextUnformatted(cur.c_str());
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                if (diff) ui::muted("%s", restore_how(k));
            }
            ImGui::EndTable();
        }
        if (diffs == 0) ui::faint("Everything matches the baseline.");
        else {
            if (ui::button_primary("Restore what FitSuite controls")) {
                const auto& b = setup.baseline;
                auto it = b.find("mousefit.active_profile");
                if (it != b.end()) {
                    setup.active_mouse_profile = it->second == "none" ? std::string() : it->second;
                    if (!setup.active_mouse_profile.empty() && !app.mouse_profile(setup.active_mouse_profile))
                        setup.active_mouse_profile.clear();
                }
                auto hz = b.find("display.refresh_hz");
                if (hz != b.end()) {
                    const double target = std::atof(hz->second.c_str());
                    if (target > 0 && std::fabs(target - setup.display.refresh_hz) > 0.5) {
                        std::string err;
                        if (app.platform().set_refresh_persistent(setup.display.device, target, &err)) {
                            AppliedChange ch;
                            ch.id = make_id("ch");
                            ch.kind = "display.refresh_rate";
                            ch.target = setup.display.device;
                            ch.before = ui::fmt("%.3f", setup.display.refresh_hz);
                            ch.after = ui::fmt("%.3f", target);
                            ch.applied_utc = utc_now_iso();
                            ch.status = "pending";
                            setup.applied_changes.push_back(ch);
                            app.motion.pending = {true, setup.display.device, setup.display.refresh_hz, target, app.time() + 15.0, ch.id, ""};
                        } else {
                            app.toast(ui::Tone::Bad, "Could not restore the refresh rate: " + err);
                        }
                    }
                }
                app.refresh_hardware();
                app.save();
                app.toast(ui::Tone::Good, "Restored the settings FitSuite controls. Change the others as listed.");
            }
            ui::faint("FitSuite never changes VALORANT, your mouse or the monitor menu; restore those yourself.");
        }
        ImGui::Dummy(ImVec2(0, 2));
        if (ui::button_secondary("Record current settings as the new baseline")) app.capture_baseline();
    }
    ui::end_card();

    ui::section("Mouse profiles");
    ui::begin_card("##pr_mouse", 0, 0);
    if (ImGui::BeginTable("##mp", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        for (const char* h : {"Profile", "Kind", "Status", "Validation", "Calibrated with", ""}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (auto& m : setup.mouse_profiles) {
            ImGui::PushID(m.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(m.label.c_str());
            if (m.origin == "simulated") ui::origin_badge(ui::Origin::Simulated);
            ImGui::TableNextColumn();
            ui::muted(m.transform.kind == TransformKind::RawAccel ? "Raw Accel-compatible" : "Smooth (in-app only)");
            ImGui::TableNextColumn();
            const bool active = setup.active_mouse_profile == m.id;
            ui::pill(active ? "SELECTED" : m.status == "discarded" ? "DISCARDED" : "CANDIDATE",
                     active ? theme::kAccent : theme::kTextMuted, active);
            ImGui::TableNextColumn();
            if (m.validation)
                ui::wrapped(m.validation->verdict == "improved"
                                ? ui::fmt("Better: %.1f%% faster aim", (1 - m.validation->time_ratio) * 100).c_str()
                                : m.validation->verdict == "worse" ? "Worse than baseline" : "No reliable difference",
                            m.validation->verdict == "improved" ? theme::kGood : theme::kTextMuted);
            else ui::faint("Not validated");
            ImGui::TableNextColumn();
            ui::wrapped(describe_conditions(m.conditions).c_str(), theme::kTextMuted);
            staleness_line(app, m.conditions, "mousefit");
            ImGui::TableNextColumn();
            if (ui::button_link("export")) {
                if (auto p = app.platform().save_file_dialog("Export mouse profile", "mouse-profile.fitsuite.json", "FitSuite mouse profile", "*.json")) {
                    std::string err;
                    if (app.store().export_mouse_profile(m, *p, &err)) app.toast(ui::Tone::Good, "Exported.");
                    else app.toast(ui::Tone::Bad, err);
                }
            }
            ImGui::SameLine();
            if (m.status == "discarded") {
                if (ui::button_link("restore")) {
                    m.status = "candidate";
                    app.save();
                }
            } else if (ui::button_link("discard")) {
                m.status = "discarded";
                if (active) setup.active_mouse_profile.clear();
                app.save();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (setup.mouse_profiles.empty()) ui::faint("No mouse profiles yet. MouseFit creates candidates after a calibration.");
    if (ui::button_secondary("Import mouse profile…")) {
        if (auto p = app.platform().open_file_dialog("Import mouse profile", "FitSuite mouse profile", "*.json")) {
            std::vector<std::string> errs;
            if (auto m = app.store().import_mouse_profile_file(*p, errs)) {
                m->id = make_id("mp");
                m->label += " (imported)";
                m->validation.reset();  // validation belongs to the install and player that ran it
                setup.mouse_profiles.push_back(*m);
                app.save();
                app.toast(ui::Tone::Good, "Imported. Validate it on this PC before using it.");
            } else {
                std::string all;
                for (const auto& e : errs) all += e + "\n";
                app.toast(ui::Tone::Bad, "The file was rejected:\n" + all, 12);
            }
        }
    }
    ui::end_card();

    ui::section("Changes made by FitSuite");
    ui::begin_card("##pr_changes", 0, 0);
    if (setup.applied_changes.empty()) ui::faint("FitSuite has not changed anything on this PC.");
    if (!setup.applied_changes.empty() &&
        ImGui::BeginTable("##ch", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        for (const char* h : {"When", "Change", "From → to", "Status", ""}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (auto it = setup.applied_changes.rbegin(); it != setup.applied_changes.rend(); ++it) {
            auto& c = *it;
            ImGui::PushID(c.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ui::muted("%s", short_time(c.applied_utc).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(c.kind == "display.refresh_rate" ? (c.temporary ? "Refresh rate (test only)" : "Refresh rate")
                                   : c.kind == "mousefit.rawaccel_export"  ? "Raw Accel settings file written"
                                   : c.kind == "mousefit.rawaccel_apply"   ? "Raw Accel: correction applied (until restart)"
                                   : c.kind == "mousefit.rawaccel_default" ? "Raw Accel: correction made default"
                                   : c.kind == "mousefit.rawaccel_games"   ? "Raw Accel: fix switched on in your games"
                                   : c.kind == "app.startup_entry"         ? "Open FitSuite when Windows starts"
                                   : c.kind == "rawaccel.settings_created" ? "Raw Accel: default settings file written"
                                                                           : c.kind.c_str());
            ImGui::TableNextColumn();
            if (c.kind == "mousefit.rawaccel_games")
                ui::wrapped(("Raw Accel switches between " + std::filesystem::path(c.before).filename().string() + " (your own) and " +
                             std::filesystem::path(c.after).filename().string() + " (with the fix) by the game in front. " +
                             "Settings file: " + c.target)
                                .c_str(),
                            theme::kTextMuted);
            else
                ui::wrapped((c.before + "  →  " + c.after).c_str(), theme::kTextMuted);
            ImGui::TableNextColumn();
            ui::pill(c.status == "applied" ? "APPLIED" : c.status == "reverted" ? "REVERTED" : c.status == "pending" ? "PENDING" : "FAILED",
                     c.status == "applied" ? theme::kAccent : theme::kTextMuted);
            ImGui::TableNextColumn();
            if ((c.kind == "mousefit.rawaccel_apply" || c.kind == "mousefit.rawaccel_default") && c.status == "applied") {
                ImGui::BeginDisabled(app.rawaccel_busy());
                if (ui::button_link("restore")) {
                    std::string err;
                    if (!app.rawaccel_restore(c, &err)) app.toast(ui::Tone::Bad, err, 10.0);
                }
                ImGui::EndDisabled();
            }
            if (c.kind == "mousefit.rawaccel_games" && c.status == "applied" && ui::button_link("turn off")) app.games_turn_off();
            if (c.kind == "app.startup_entry" && c.status == "applied" && ui::button_link("remove")) app.set_start_with_windows(false);
            if (c.kind == "display.refresh_rate" && c.status == "applied" && !c.temporary) {
                if (ui::button_link("revert")) {
                    std::string err;
                    if (app.platform().set_refresh_persistent(c.target, std::atof(c.before.c_str()), &err)) {
                        c.status = "reverted";
                        c.reverted_utc = utc_now_iso();
                        app.refresh_hardware();
                        app.save();
                    } else {
                        app.toast(ui::Tone::Bad, err);
                    }
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ui::end_card();
}

}  // namespace fitapp
