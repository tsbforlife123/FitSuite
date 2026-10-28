// Visual language: a plain Windows utility in the style of Raw Accel, in light or dark. Light: white
// pages, a light grey tab strip and status bar, black text. Dark: the same layout in charcoal greys
// with light text. Square corners, one blue for actions, and colour only where it means something
// (good / warning / bad / estimate). Segoe UI on Windows (the system font), the embedded Inter
// elsewhere. The full-screen test scenes keep their own dark palette in both themes.
#pragma once

#include "imgui.h"

namespace fitapp::theme {

inline ImU32 rgb(unsigned hex, float a = 1.0f) {
    return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, int(a * 255.0f + 0.5f));
}
inline ImVec4 v4(unsigned hex, float a = 1.0f) {
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, a);
}

// The current palette, switched by apply() between light (the values below) and dark. Pages read
// these at draw time, so a theme change shows on the next frame.
// Surfaces
inline unsigned kBg0 = 0xF0F0F0;      // tab strip, status bar, table headers (Windows "Control")
inline unsigned kBg1 = 0xFFFFFF;      // page background
inline unsigned kCard = 0xFFFFFF;     // group boxes: same as the page, framed by a line
inline unsigned kCardHi = 0xFFFFFF;   // popups, tooltips, selected tab
inline unsigned kInset = 0xFFFFFF;    // text boxes, lists, chart areas (framed by kBorderHi)
inline unsigned kBorder = 0xD9D9D9;   // group boxes, tables, separators
inline unsigned kBorderHi = 0xA0A0A0; // text boxes, buttons, tabs
// Text
inline unsigned kText = 0x1B1B1B;
inline unsigned kTextMuted = 0x4F4F4F;
inline unsigned kTextFaint = 0x6B6B6B;
inline unsigned kTextDisabled = 0x9A9A9A;
// Meaning
inline unsigned kAccent = 0x0067C0;   // actions, links, measured data
inline unsigned kAccentDim = 0xCCE4F7;// selection, pressed
inline unsigned kGood = 0x107C10;
inline unsigned kWarn = 0x9D5D00;     // warnings, preferences (subjective)
inline unsigned kBad = 0xC42B1C;      // problems, simulated / demo data
inline unsigned kInfo = 0x00707E;     // estimates, derived values
inline unsigned kOrange = 0xCA5010;   // second chart series
// Controls
inline unsigned kHover = 0xE5F1FB;    // hovered tab, list row or button
inline unsigned kButton = 0xE1E1E1, kButtonBorder = 0xADADAD, kButtonHoverBorder = 0x0078D7;
inline unsigned kButtonOff = 0xEFEFEF, kButtonOffText = 0x9A9A9A, kButtonOffBorder = 0xC9C9C9;
inline unsigned kPrimary = 0x0067C0, kPrimaryHover = 0x1975C5, kPrimaryActive = 0x005A9E, kPrimaryText = 0xFFFFFF;
inline unsigned kPrimaryBorder = 0x005A9E, kPrimaryHoverBorder = 0x003E73;
inline unsigned kPrimaryOff = 0xCCCCCC, kPrimaryOffText = 0x8A8A8A, kPrimaryOffBorder = 0xBFBFBF;
inline unsigned kDangerHover = 0xFDE7E9, kDangerActive = 0xF9D3D6;
inline unsigned kTrack = 0xE6E6E6, kTrackBorder = 0xBCBCBC;  // progress bars
inline unsigned kSpoke = 0x262B32;    // direction wheel spokes
// Info bars: background and border per tone
inline unsigned kInfoBg = 0xEFF6FC, kInfoBorder = 0xA9CDEB;
inline unsigned kGoodBg = 0xDFF6DD, kGoodBorder = 0x9ED49E;
inline unsigned kWarnBg = 0xFFF4CE, kWarnBorder = 0xE3CB7C;
inline unsigned kBadBg = 0xFDE7E9, kBadBorder = 0xEBA7AE;
inline unsigned kNeutralBg = 0xF5F5F5;
// Full-screen test scenes (dark, so targets and motion stand out)
inline constexpr unsigned kSceneBg = 0x0D0F12;
inline constexpr unsigned kSceneGrid = 0x2A3038;
inline constexpr unsigned kSceneHorizon = 0x4A5562;
inline constexpr unsigned kTarget = 0xF2F4F7;
inline constexpr unsigned kCrosshair = 0x6CF2C2;
inline constexpr unsigned kSceneBar = 0x232930;
inline constexpr unsigned kSceneText = 0xE6E9ED;
inline constexpr unsigned kSceneMuted = 0x9BA5B0;
inline constexpr unsigned kSceneFaint = 0x6A747F;
inline constexpr unsigned kSceneAccent = 0x45C4B0;
inline constexpr unsigned kSceneGood = 0x6CCB8A;
inline constexpr unsigned kSceneWarn = 0xE8B44D;
inline constexpr unsigned kSceneBad = 0xE5695B;

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* medium = nullptr;
    ImFont* semibold = nullptr;
    ImFont* mono = nullptr;
    ImFont* mono_medium = nullptr;
};
Fonts& fonts();

inline constexpr float kBody = 14.0f;
inline constexpr float kSmall = 12.5f;
inline constexpr float kH1 = 20.0f;
inline constexpr float kH2 = 16.0f;
inline constexpr float kH3 = 14.0f;
inline constexpr float kMono = 13.0f;
inline constexpr float kMetric = 20.0f;

// Loads the fonts and applies the style (light). `dpi_scale` multiplies all sizes.
void setup(float dpi_scale);

// Switches all colours between light and dark (palette above, Dear ImGui and ImPlot). Call between
// frames or at the very start of one.
void apply(bool dark);
bool is_dark();

}  // namespace fitapp::theme
