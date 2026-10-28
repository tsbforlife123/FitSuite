#include "widgets.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui_internal.h"

namespace fitapp::ui {

using namespace theme;

const char* origin_label(Origin o) {
    switch (o) {
        case Origin::Measured: return "MEASURED";
        case Origin::Estimate: return "ESTIMATE";
        case Origin::Preference: return "PREFERENCE";
        case Origin::UserEntered: return "ENTERED";
        case Origin::Detected: return "DETECTED";
        case Origin::Simulated: return "SIMULATED DATA";
        case Origin::NotAvailable: return "NOT AVAILABLE";
        case Origin::None: return "";
    }
    return "";
}

static unsigned origin_color(Origin o) {
    switch (o) {
        case Origin::Measured: return kAccent;
        case Origin::Estimate: return kInfo;
        case Origin::Preference: return kWarn;
        case Origin::UserEntered: return kTextMuted;
        case Origin::Detected: return kAccent;
        case Origin::Simulated: return kBad;
        case Origin::NotAvailable: return kTextFaint;
        case Origin::None: return kTextFaint;
    }
    return kTextMuted;
}

void push_font(ImFont* f, float size) { ImGui::PushFont(f, size); }
void pop_font() { ImGui::PopFont(); }

std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string num(double v, int decimals) {
    if (!std::isfinite(v)) return "—";
    return fmt("%.*f", decimals, v);
}

std::string signed_num(double v, int decimals) {
    if (!std::isfinite(v)) return "—";
    return fmt("%+.*f", decimals, v);
}

// The tab already names the page, so a page starts with one plain line saying what it is for.
void page_header(const char* /*title*/, const char* subtitle) {
    if (subtitle && *subtitle) {
        ImGui::PushStyleColor(ImGuiCol_Text, v4(kTextMuted));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(subtitle);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 2));
}

// Bold caption followed by a thin line to the right edge, like a Windows group heading.
void section(const char* title) {
    ImGui::Dummy(ImVec2(0, 2));
    const float x1 = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    push_font(fonts().semibold, kBody);
    ImGui::TextUnformatted(title);
    pop_font();
    const ImVec2 a = ImGui::GetItemRectMax();
    const float y = (ImGui::GetItemRectMin().y + a.y) * 0.5f;
    if (x1 > a.x + 8) ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x + 8, y), ImVec2(x1, y), rgb(kBorder));
}

// A group box: a thin frame around related controls, same background as the page.
bool begin_card(const char* id, float width, float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(kCard));
    ImGui::PushStyleColor(ImGuiCol_Border, v4(kBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGuiChildFlags flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
    if (height <= 0.0f) flags |= ImGuiChildFlags_AutoResizeY;
    const bool open = ImGui::BeginChild(id, ImVec2(width, height > 0.0f ? height : 0.0f), flags,
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleColor(ImGuiCol_Border, v4(kBorderHi));  // controls inside keep their normal frame
    return open;
}

void end_card() {
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

void card_title(const char* title, const char* subtitle) {
    push_font(fonts().semibold, kBody);
    ImGui::TextUnformatted(title);
    pop_font();
    if (subtitle && *subtitle) {
        ImGui::PushStyleColor(ImGuiCol_Text, v4(kTextMuted));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(subtitle);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 1));
}

// A short status word in colour (e.g. MEASURED, DONE). Plain text, no badge shape.
void pill(const char* text, unsigned color, bool /*filled*/) {
    push_font(fonts().semibold, kSmall);
    const float line_h = ImGui::GetTextLineHeight();
    pop_font();
    const float body_h = ImGui::GetTextLineHeight();
    push_font(fonts().semibold, kSmall);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, p.y + (body_h - line_h) * 0.5f + 1.0f), rgb(color), text);
    ImGui::Dummy(ImVec2(ts.x, body_h));
    pop_font();
}

void origin_badge(Origin o) {
    if (o == Origin::None) return;
    pill(origin_label(o), origin_color(o), o == Origin::Simulated || o == Origin::Measured);
}

void metric(const char* label, const std::string& value, const char* unit, Origin o, unsigned value_color) {
    ImGui::BeginGroup();
    push_font(fonts().semibold, kMetric);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(value_color));
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopStyleColor();
    pop_font();
    if (unit && *unit) {
        ImGui::SameLine(0, 4);
        push_font(fonts().regular, kSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(kTextMuted));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6);
        ImGui::TextUnformatted(unit);
        ImGui::PopStyleColor();
        pop_font();
    }
    push_font(fonts().regular, kSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(kTextMuted));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    pop_font();
    origin_badge(o);
    ImGui::EndGroup();
}

void kv(const char* key, const std::string& value, unsigned value_color) {
    ImGui::PushStyleColor(ImGuiCol_Text, v4(kTextMuted));
    ImGui::TextUnformatted(key);
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetCursorPosX() + 160.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(value_color));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(value.empty() ? "—" : value.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static void vtext(unsigned color, const char* f, va_list ap) {
    char buf[2048];
    std::vsnprintf(buf, sizeof buf, f, ap);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(color));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void muted(const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    vtext(kTextMuted, f, ap);
    va_end(ap);
}

void faint(const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    vtext(kTextFaint, f, ap);
    va_end(ap);
}

void wrapped(const char* text, unsigned color) {
    ImGui::PushStyleColor(ImGuiCol_Text, v4(color));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void help_marker(const char* text) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, v4(kTextFaint));
    ImGui::TextUnformatted("(?)");
    ImGui::PopStyleColor();
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// A plain message bar (like Windows' info bars): tinted box, thin border, bold first line.
void callout(Tone tone, const char* title, const char* text) {
    unsigned fg = kAccent, bg = kInfoBg, border = kInfoBorder;
    switch (tone) {
        case Tone::Info: break;
        case Tone::Good: fg = kGood, bg = kGoodBg, border = kGoodBorder; break;
        case Tone::Warn: fg = kWarn, bg = kWarnBg, border = kWarnBorder; break;
        case Tone::Bad: fg = kBad, bg = kBadBg, border = kBadBorder; break;
        case Tone::Neutral: fg = kText, bg = kNeutralBg, border = kBorder; break;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, 1));
    ImGui::Indent(10);
    if (title && *title) {
        push_font(fonts().semibold, kBody);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(fg));
        ImGui::PushTextWrapPos(start.x + w - 10.0f - ImGui::GetWindowPos().x + ImGui::GetScrollX());
        ImGui::TextUnformatted(title);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        pop_font();
    }
    if (text && *text) {
        ImGui::PushTextWrapPos(start.x + w - 10.0f - ImGui::GetWindowPos().x + ImGui::GetScrollX());
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
    }
    ImGui::Unindent(10);
    ImGui::Dummy(ImVec2(0, 1));
    ImGui::EndGroup();
    const ImVec2 end(start.x + w, ImGui::GetItemRectMax().y + 2);
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(start, end, rgb(bg));
    dl->AddRect(start, end, rgb(border));
    dl->ChannelsMerge();
    ImGui::Dummy(ImVec2(0, 3));
}

// Windows-style push buttons: at least 80 px wide, a blue frame on hover.
static bool styled_button(const char* label, ImVec2 size, unsigned bg, unsigned bg_hover, unsigned bg_active, unsigned text,
                          unsigned border, unsigned hover_border) {
    ImGui::PushStyleColor(ImGuiCol_Button, v4(bg));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(bg_hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, v4(bg_active));
    ImGui::PushStyleColor(ImGuiCol_Text, v4(text));
    ImGui::PushStyleColor(ImGuiCol_Border, v4(border));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 4));
    if (size.x == 0.0f) {
        const char* end = ImGui::FindRenderedTextEnd(label);
        const float w = ImGui::CalcTextSize(label, end).x + 24.0f;
        if (w < 80.0f) size.x = 80.0f;
    }
    const bool disabled = (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    if (disabled) ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);  // the colours above already look disabled
    const bool r = ImGui::Button(label, size);
    if (disabled) ImGui::PopStyleVar();
    if (ImGui::IsItemHovered() && !disabled)
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), rgb(hover_border));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    return r;
}

// Inside BeginDisabled(): the next item will be drawn disabled.
static bool next_item_disabled() { return (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0; }

bool button_primary(const char* label, ImVec2 size) {
    if (next_item_disabled())
        return styled_button(label, size, kPrimaryOff, kPrimaryOff, kPrimaryOff, kPrimaryOffText, kPrimaryOffBorder,
                             kPrimaryOffBorder);
    return styled_button(label, size, kPrimary, kPrimaryHover, kPrimaryActive, kPrimaryText, kPrimaryBorder,
                         kPrimaryHoverBorder);
}
bool button_secondary(const char* label, ImVec2 size) {
    if (next_item_disabled())
        return styled_button(label, size, kButtonOff, kButtonOff, kButtonOff, kButtonOffText, kButtonOffBorder, kButtonOffBorder);
    return styled_button(label, size, kButton, kHover, kAccentDim, kText, kButtonBorder, kButtonHoverBorder);
}
bool button_danger(const char* label, ImVec2 size) {
    return styled_button(label, size, kButton, kDangerHover, kDangerActive, kBad, kButtonBorder, kBad);
}

bool button_link(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Text, v4(kAccent));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y), b, rgb(kAccent, 0.7f));
    }
    return hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

bool confirm_link(const char* label, const char* question) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID id = ImGui::GetID(label);
    bool yes = false;
    ImGui::PushID(label);
    if (!st->GetBool(id, false)) {
        if (button_link(label)) st->SetBool(id, true);
    } else {
        ImGui::TextUnformatted(question);
        ImGui::SameLine(0, 8);
        if (button_link("yes")) {
            yes = true;
            st->SetBool(id, false);
        }
        ImGui::SameLine(0, 6);
        ImGui::TextDisabled("·");
        ImGui::SameLine(0, 6);
        if (button_link("no")) st->SetBool(id, false);
    }
    ImGui::PopID();
    return yes;
}

void progress_bar(float f, const char* overlay, float width, float height) {
    f = f < 0 ? 0 : (f > 1 ? 1 : f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = width < 0 ? ImGui::GetContentRegionAvail().x : width;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + height), rgb(kTrack));
    if (f > 0) dl->AddRectFilled(p, ImVec2(p.x + w * f, p.y + height), rgb(kAccent));
    dl->AddRect(p, ImVec2(p.x + w, p.y + height), rgb(kTrackBorder));
    ImGui::Dummy(ImVec2(w, height));
    if (overlay && *overlay) muted("%s", overlay);
}

// Classic tab strip: the selected tab is white and open towards the page below it.
int tabs(const char* const* labels, int count, int current) {
    int clicked = -1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float pad_x = 14.0f, pad_y = 5.0f;
    const float h = ImGui::GetTextLineHeight() + pad_y * 2;
    float x = origin.x;
    float sel_a = -1.0f, sel_b = -1.0f;
    for (int i = 0; i < count; ++i) {
        const bool sel = i == current;
        const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        const ImVec2 a(x, origin.y + (sel ? 0.0f : 2.0f)), b(x + ts.x + pad_x * 2, origin.y + h);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("tab", ImVec2(b.x - a.x, b.y - a.y))) clicked = i;
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (sel) {
            dl->AddRectFilled(a, b, rgb(kCardHi));
            dl->AddLine(ImVec2(a.x, b.y), a, rgb(kBorderHi));
            dl->AddLine(a, ImVec2(b.x, a.y), rgb(kBorderHi));
            dl->AddLine(ImVec2(b.x, a.y), b, rgb(kBorderHi));
            sel_a = a.x;
            sel_b = b.x;
        } else if (hovered) {
            dl->AddRectFilled(a, b, rgb(kHover));
        }
        dl->AddText(ImVec2(a.x + pad_x, a.y + pad_y - (sel ? 0.0f : 1.0f)), rgb(sel ? kText : kTextMuted), labels[i]);
        x = b.x + (sel ? 0.0f : 0.0f);
    }
    // Baseline under the tabs, open under the selected one.
    const float y = origin.y + h;
    if (sel_a < 0) dl->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + width, y), rgb(kBorderHi));
    else {
        dl->AddLine(ImVec2(origin.x, y), ImVec2(sel_a, y), rgb(kBorderHi));
        dl->AddLine(ImVec2(sel_b, y), ImVec2(origin.x + width, y), rgb(kBorderHi));
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y + 1));
    ImGui::Dummy(ImVec2(width, 6));
    return clicked;
}

// Numbered steps shown as tabs ("1 Setup", "2 Calibrate", …).
int steps(const char* const* labels, int count, int current, const bool* /*done*/) {
    std::vector<std::string> text;
    std::vector<const char*> ptrs;
    for (int i = 0; i < count; ++i) text.push_back(fmt("%d  %s", i + 1, labels[i]));
    for (const auto& t : text) ptrs.push_back(t.c_str());
    return tabs(ptrs.data(), count, current);
}

}  // namespace fitapp::ui
