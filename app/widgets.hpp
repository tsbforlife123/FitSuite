// Small UI building blocks shared by all pages, so every screen uses the same visual language.
#pragma once

#include <string>

#include "imgui.h"
#include "theme.hpp"

namespace fitapp::ui {

// What kind of number is shown. Every value on screen is tagged with one of these.
enum class Origin {
    Measured,     // recorded on this PC in a test
    Estimate,     // derived/modelled, e.g. in-sample prediction
    Preference,   // the player's own rating or choice
    UserEntered,  // typed in by the player (e.g. DPI)
    Detected,     // read from Windows
    Simulated,    // synthetic data (development harness / tests only)
    NotAvailable,
    None,         // origin shown once for a whole group (e.g. in its header), so no badge here
};
const char* origin_label(Origin o);

void push_font(ImFont* f, float size);
void pop_font();

void page_header(const char* title, const char* subtitle);
void section(const char* title);
// Card = a group box: thin frame, page background. Height 0 = auto-fit to content.
bool begin_card(const char* id, float width = 0.0f, float height = 0.0f);
void end_card();
void card_title(const char* title, const char* subtitle = nullptr);

void pill(const char* text, unsigned color, bool filled = false);
void origin_badge(Origin o);
// A large mono number with unit and a label underneath.
void metric(const char* label, const std::string& value, const char* unit, Origin o, unsigned value_color = theme::kText);
void kv(const char* key, const std::string& value, unsigned value_color = theme::kText);
void muted(const char* fmt, ...);
void faint(const char* fmt, ...);
void wrapped(const char* text, unsigned color = theme::kText);
void help_marker(const char* text);

enum class Tone { Info, Good, Warn, Bad, Neutral };
void callout(Tone tone, const char* title, const char* text);

bool button_primary(const char* label, ImVec2 size = ImVec2(0, 0));
bool button_secondary(const char* label, ImVec2 size = ImVec2(0, 0));
bool button_danger(const char* label, ImVec2 size = ImVec2(0, 0));
bool button_link(const char* label);
// A link that asks before acting, in place and without a pop-up: "delete" becomes
// "Delete this run? yes · no". Returns true when "yes" is clicked. The open/closed state is kept by
// Dear ImGui under the current ID stack, so each table row needs its own PushID.
bool confirm_link(const char* label, const char* question);

void progress_bar(float fraction, const char* overlay, float width = -1.0f, float height = 8.0f);
// Numbered steps shown as tabs ("1 Setup", "2 Calibrate", …). Returns the clicked step or -1.
int steps(const char* const* labels, int count, int current, const bool* done = nullptr);
// Classic tab strip. Returns the clicked tab or -1.
int tabs(const char* const* labels, int count, int current);

std::string fmt(const char* f, ...);
std::string num(double v, int decimals);
std::string signed_num(double v, int decimals);

}  // namespace fitapp::ui
