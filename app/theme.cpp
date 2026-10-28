#include "theme.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "implot.h"

namespace fitapp::fonts {
extern const unsigned char* const Inter_Regular;
extern const size_t Inter_Regular_size;
extern const unsigned char* const Inter_Medium;
extern const size_t Inter_Medium_size;
extern const unsigned char* const Inter_SemiBold;
extern const size_t Inter_SemiBold_size;
extern const unsigned char* const IBMPlexMono_Regular;
extern const size_t IBMPlexMono_Regular_size;
extern const unsigned char* const IBMPlexMono_Medium;
extern const size_t IBMPlexMono_Medium_size;
}  // namespace fitapp::fonts

namespace fitapp::theme {

Fonts& fonts() {
    static Fonts f;
    return f;
}

namespace {

ImFont* load_embedded(const unsigned char* data, size_t size, const char* name, bool merge) {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // embedded, static data
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.MergeMode = merge;
    std::snprintf(cfg.Name, sizeof cfg.Name, "%s", name);
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), int(size), kBody, &cfg);
}

// The Windows system font when it is there (so FitSuite looks like the rest of Windows), with the
// embedded font merged in behind it for any character the system font lacks. Elsewhere (the
// development harness) the embedded font alone.
ImFont* load(const char* system_file, const unsigned char* data, size_t size, const char* name) {
    if (const char* windir = std::getenv("WINDIR")) {
        const std::string path = std::string(windir) + "\\Fonts\\" + system_file;
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            cfg.OversampleV = 1;
            std::snprintf(cfg.Name, sizeof cfg.Name, "%s", system_file);
            if (ImFont* f = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), kBody, &cfg)) {
                load_embedded(data, size, name, true);
                return f;
            }
        }
    }
    return load_embedded(data, size, name, false);
}

}  // namespace

namespace {

bool g_dark = false;
ImPlotColormap g_cmap_light = -1, g_cmap_dark = -1;

struct Palette {
    unsigned bg0, bg1, card, card_hi, inset, border, border_hi;
    unsigned text, text_muted, text_faint, text_disabled;
    unsigned accent, accent_dim, good, warn, bad, info, orange;
    unsigned hover, button, button_border, button_hover_border, button_off, button_off_text, button_off_border;
    unsigned primary, primary_hover, primary_active, primary_text, primary_border, primary_hover_border;
    unsigned primary_off, primary_off_text, primary_off_border, danger_hover, danger_active;
    unsigned track, track_border, spoke;
    unsigned info_bg, info_border, good_bg, good_border, warn_bg, warn_border, bad_bg, bad_border, neutral_bg;
};

// Windows "light" controls, as in Raw Accel.
constexpr Palette kLight = {
    0xF0F0F0, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xD9D9D9, 0xA0A0A0,
    0x1B1B1B, 0x4F4F4F, 0x6B6B6B, 0x9A9A9A,
    0x0067C0, 0xCCE4F7, 0x107C10, 0x9D5D00, 0xC42B1C, 0x00707E, 0xCA5010,
    0xE5F1FB, 0xE1E1E1, 0xADADAD, 0x0078D7, 0xEFEFEF, 0x9A9A9A, 0xC9C9C9,
    0x0067C0, 0x1975C5, 0x005A9E, 0xFFFFFF, 0x005A9E, 0x003E73,
    0xCCCCCC, 0x8A8A8A, 0xBFBFBF, 0xFDE7E9, 0xF9D3D6,
    0xE6E6E6, 0xBCBCBC, 0x262B32,
    0xEFF6FC, 0xA9CDEB, 0xDFF6DD, 0x9ED49E, 0xFFF4CE, 0xE3CB7C, 0xFDE7E9, 0xEBA7AE, 0xF5F5F5,
};

// The same layout in charcoal greys (Windows dark mode), text never pure white.
constexpr Palette kDark = {
    0x2B2B2B, 0x1F1F1F, 0x1F1F1F, 0x2B2B2B, 0x2A2A2A, 0x3D3D3D, 0x6E6E6E,
    0xE6E6E6, 0xB3B3B3, 0x8F8F8F, 0x6B6B6B,
    0x4CA6FF, 0x1F3A57, 0x6CCB5F, 0xE0B048, 0xF28B82, 0x4FC3CF, 0xF0A050,
    0x333A44, 0x333333, 0x5C5C5C, 0x4CA6FF, 0x2A2A2A, 0x6B6B6B, 0x3D3D3D,
    0x2F6FC2, 0x3A7DD0, 0x285FA8, 0xF5F5F5, 0x285FA8, 0x7FB6F2,
    0x2E2E2E, 0x6B6B6B, 0x3D3D3D, 0x3D2326, 0x4A2A2E,
    0x333333, 0x5C5C5C, 0xA0A6AD,
    0x1C2A38, 0x2F5378, 0x1D2B1D, 0x355E35, 0x32291A, 0x6B5628, 0x372023, 0x6E3439, 0x262626,
};

void set_palette(const Palette& p) {
    kBg0 = p.bg0, kBg1 = p.bg1, kCard = p.card, kCardHi = p.card_hi, kInset = p.inset, kBorder = p.border;
    kBorderHi = p.border_hi;
    kText = p.text, kTextMuted = p.text_muted, kTextFaint = p.text_faint, kTextDisabled = p.text_disabled;
    kAccent = p.accent, kAccentDim = p.accent_dim, kGood = p.good, kWarn = p.warn, kBad = p.bad, kInfo = p.info;
    kOrange = p.orange;
    kHover = p.hover, kButton = p.button, kButtonBorder = p.button_border, kButtonHoverBorder = p.button_hover_border;
    kButtonOff = p.button_off, kButtonOffText = p.button_off_text, kButtonOffBorder = p.button_off_border;
    kPrimary = p.primary, kPrimaryHover = p.primary_hover, kPrimaryActive = p.primary_active, kPrimaryText = p.primary_text;
    kPrimaryBorder = p.primary_border, kPrimaryHoverBorder = p.primary_hover_border;
    kPrimaryOff = p.primary_off, kPrimaryOffText = p.primary_off_text, kPrimaryOffBorder = p.primary_off_border;
    kDangerHover = p.danger_hover, kDangerActive = p.danger_active;
    kTrack = p.track, kTrackBorder = p.track_border, kSpoke = p.spoke;
    kInfoBg = p.info_bg, kInfoBorder = p.info_border, kGoodBg = p.good_bg, kGoodBorder = p.good_border;
    kWarnBg = p.warn_bg, kWarnBorder = p.warn_border, kBadBg = p.bad_bg, kBadBorder = p.bad_border;
    kNeutralBg = p.neutral_bg;
}

}  // namespace

bool is_dark() { return g_dark; }

void apply(bool dark) {
    g_dark = dark;
    set_palette(dark ? kDark : kLight);

    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = v4(kText);
    c[ImGuiCol_TextDisabled] = v4(kTextDisabled);
    c[ImGuiCol_WindowBg] = v4(kBg1);
    c[ImGuiCol_ChildBg] = v4(kCard, 0.0f);
    c[ImGuiCol_PopupBg] = v4(kCardHi);
    c[ImGuiCol_Border] = v4(kBorderHi);  // buttons and text boxes; group boxes use the lighter kBorder
    c[ImGuiCol_BorderShadow] = v4(0x000000, 0.0f);
    c[ImGuiCol_FrameBg] = v4(kInset);
    c[ImGuiCol_FrameBgHovered] = v4(kInset);
    c[ImGuiCol_FrameBgActive] = v4(kInset);
    c[ImGuiCol_TitleBg] = v4(kBg0);
    c[ImGuiCol_TitleBgActive] = v4(kBg0);
    c[ImGuiCol_TitleBgCollapsed] = v4(kBg0);
    c[ImGuiCol_MenuBarBg] = v4(kBg0);
    c[ImGuiCol_ScrollbarBg] = v4(kBg0);
    c[ImGuiCol_ScrollbarGrab] = v4(dark ? 0x5A5A5A : 0xC2C2C2);
    c[ImGuiCol_ScrollbarGrabHovered] = v4(dark ? 0x6E6E6E : 0xA6A6A6);
    c[ImGuiCol_ScrollbarGrabActive] = v4(dark ? 0x858585 : 0x8C8C8C);
    c[ImGuiCol_CheckMark] = v4(kText);
    c[ImGuiCol_CheckboxSelectedBg] = v4(kInset);  // a ticked box keeps its normal fill, like a Windows check box
    c[ImGuiCol_SliderGrab] = v4(kAccent);
    c[ImGuiCol_SliderGrabActive] = v4(kPrimaryActive);
    c[ImGuiCol_Button] = v4(kButton);
    c[ImGuiCol_ButtonHovered] = v4(kHover);
    c[ImGuiCol_ButtonActive] = v4(kAccentDim);
    c[ImGuiCol_Header] = v4(kAccentDim);
    c[ImGuiCol_HeaderHovered] = v4(kHover);
    c[ImGuiCol_HeaderActive] = v4(kAccentDim);
    c[ImGuiCol_Separator] = v4(kBorder);
    c[ImGuiCol_SeparatorHovered] = v4(kBorderHi);
    c[ImGuiCol_SeparatorActive] = v4(kAccent);
    c[ImGuiCol_ResizeGrip] = v4(0x000000, 0.0f);
    c[ImGuiCol_ResizeGripHovered] = v4(kBorderHi);
    c[ImGuiCol_ResizeGripActive] = v4(kAccent);
    c[ImGuiCol_InputTextCursor] = v4(kText);
    c[ImGuiCol_Tab] = v4(kBg0);
    c[ImGuiCol_TabHovered] = v4(kHover);
    c[ImGuiCol_TabSelected] = v4(kCardHi);
    c[ImGuiCol_TabSelectedOverline] = v4(kCardHi, 0.0f);
    c[ImGuiCol_TabDimmed] = v4(kBg0);
    c[ImGuiCol_TabDimmedSelected] = v4(kCardHi);
    c[ImGuiCol_TabDimmedSelectedOverline] = v4(kCardHi, 0.0f);
    c[ImGuiCol_PlotLines] = v4(kAccent);
    c[ImGuiCol_PlotLinesHovered] = v4(kPrimaryActive);
    c[ImGuiCol_PlotHistogram] = v4(kAccent);
    c[ImGuiCol_PlotHistogramHovered] = v4(kPrimaryActive);
    c[ImGuiCol_TableHeaderBg] = v4(kBg0);
    c[ImGuiCol_TableBorderStrong] = v4(kBorder);
    c[ImGuiCol_TableBorderLight] = v4(dark ? 0x333333 : 0xE6E6E6);
    c[ImGuiCol_TableRowBg] = v4(0x000000, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = dark ? v4(0xFFFFFF, 0.03f) : v4(0x000000, 0.025f);
    c[ImGuiCol_TextLink] = v4(kAccent);
    c[ImGuiCol_TreeLines] = v4(kBorder);
    c[ImGuiCol_TextSelectedBg] = v4(kAccent, 0.30f);
    c[ImGuiCol_DragDropTarget] = v4(kAccent);
    c[ImGuiCol_NavCursor] = v4(kAccent);
    c[ImGuiCol_NavWindowingHighlight] = v4(kAccent, 0.6f);
    c[ImGuiCol_NavWindowingDimBg] = v4(0x000000, 0.2f);
    c[ImGuiCol_ModalWindowDimBg] = v4(0x000000, 0.35f);

    ImPlotStyle& p = ImPlot::GetStyle();
    if (g_cmap_light < 0) {
        static const ImU32 light[] = {rgb(kLight.accent), rgb(kLight.orange), rgb(kLight.info), rgb(kLight.bad), rgb(kLight.good),
                                      rgb(0x6B6B6B)};
        static const ImU32 darkc[] = {rgb(kDark.accent), rgb(kDark.orange), rgb(kDark.info), rgb(kDark.bad), rgb(kDark.good),
                                      rgb(0xA0A0A0)};
        g_cmap_light = ImPlot::AddColormap("FitSuite light", light, 6, true);
        g_cmap_dark = ImPlot::AddColormap("FitSuite dark", darkc, 6, true);
    }
    p.Colormap = dark ? g_cmap_dark : g_cmap_light;
    ImVec4* pc = p.Colors;
    pc[ImPlotCol_FrameBg] = v4(0x000000, 0.0f);
    pc[ImPlotCol_PlotBg] = v4(kInset);
    pc[ImPlotCol_PlotBorder] = v4(kBorderHi);
    pc[ImPlotCol_LegendBg] = v4(kCardHi, 0.95f);
    pc[ImPlotCol_LegendBorder] = v4(kBorder);
    pc[ImPlotCol_LegendText] = v4(kText);
    pc[ImPlotCol_TitleText] = v4(kText);
    pc[ImPlotCol_InlayText] = v4(kTextMuted);
    pc[ImPlotCol_AxisText] = v4(kTextMuted);
    pc[ImPlotCol_AxisGrid] = dark ? v4(0xFFFFFF, 0.08f) : v4(0x000000, 0.09f);
    pc[ImPlotCol_AxisTick] = v4(dark ? 0x7A7A7A : 0x8C8C8C);
    pc[ImPlotCol_Selection] = v4(kWarn);
    pc[ImPlotCol_Crosshairs] = v4(kTextMuted);
}

void setup(float dpi_scale) {
    ImGuiIO& io = ImGui::GetIO();
    auto& f = fonts();
    using namespace fitapp::fonts;
    f.regular = load("segoeui.ttf", Inter_Regular, Inter_Regular_size, "Inter Regular");
    f.medium = load("seguisb.ttf", Inter_Medium, Inter_Medium_size, "Inter Medium");
    f.semibold = load("seguisb.ttf", Inter_SemiBold, Inter_SemiBold_size, "Inter SemiBold");
    f.mono = load("consola.ttf", IBMPlexMono_Regular, IBMPlexMono_Regular_size, "IBM Plex Mono");
    f.mono_medium = load("consolab.ttf", IBMPlexMono_Medium, IBMPlexMono_Medium_size, "IBM Plex Mono Medium");
    io.FontDefault = f.regular;

    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsLight(&s);  // defaults for anything apply() does not set
    s.FontSizeBase = kBody;
    s.FontScaleDpi = dpi_scale;
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(7, 4);
    s.CellPadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 7);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing = 16;
    s.ScrollbarSize = 16;
    s.GrabMinSize = 12;
    s.WindowBorderSize = 1;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 1;
    s.TabBorderSize = 1;
    s.WindowRounding = 0;
    s.ChildRounding = 0;
    s.FrameRounding = 0;
    s.PopupRounding = 0;
    s.ScrollbarRounding = 0;
    s.GrabRounding = 0;
    s.TabRounding = 0;
    s.SeparatorTextBorderSize = 1;
    s.SelectableTextAlign = ImVec2(0.0f, 0.5f);

    ImPlotStyle& p = ImPlot::GetStyle();
    p.PlotPadding = ImVec2(8, 8);
    p.LabelPadding = ImVec2(5, 3);
    p.LegendPadding = ImVec2(8, 6);
    p.LegendInnerPadding = ImVec2(5, 3);
    p.PlotBorderSize = 1.0f;
    p.FitPadding = ImVec2(0.02f, 0.10f);  // headroom so markers at the extremes are not cut off
    p.MinorAlpha = 0.25f;
    apply(g_dark);
}

}  // namespace fitapp::theme
