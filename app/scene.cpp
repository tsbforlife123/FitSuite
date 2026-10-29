#include "scene.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "fit/common.hpp"
#include "theme.hpp"

namespace fitapp {

using fit::kDegToRad;

Dir3 direction_from(double yaw_deg, double pitch_deg) {
    const double y = yaw_deg * kDegToRad, p = pitch_deg * kDegToRad;
    return {std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y)};
}

double angular_distance_deg(double ya, double pa, double yb, double pb) {
    const Dir3 a = direction_from(ya, pa), b = direction_from(yb, pb);
    const double d = std::clamp(a.x * b.x + a.y * b.y + a.z * b.z, -1.0, 1.0);
    return std::acos(d) / kDegToRad;
}

static double dotd(const Dir3& a, const Dir3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

void SceneRenderer::begin(ImDrawList* dl, ImVec2 origin, ImVec2 size, const Camera& cam) {
    dl_ = dl;
    origin_ = origin;
    size_ = size;
    const double y = cam.yaw_deg * kDegToRad, p = cam.pitch_deg * kDegToRad;
    fwd_ = {std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y)};
    right_ = {std::cos(y), 0.0, -std::sin(y)};
    up_ = {-std::sin(p) * std::sin(y), std::cos(p), -std::sin(p) * std::cos(y)};
    // The main game's horizontal FOV at 16:9 (VALORANT 103°); other screen shapes keep the 16:9 vertical
    // FOV, as these games do, so the test matches the player's monitor and game.
    const double vfov = 2.0 * std::atan(std::tan(cam.hfov_deg_16x9 * kDegToRad / 2.0) * 9.0 / 16.0);
    fy_ = (size.y * 0.5) / std::tan(vfov / 2.0);
    fx_ = fy_;
    dl_->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), theme::rgb(theme::kSceneBg));
}

bool SceneRenderer::project(const Dir3& d, ImVec2& out, double* depth) const {
    const double z = dotd(d, fwd_);
    if (depth) *depth = z;
    if (z <= 0.02) return false;
    const double x = dotd(d, right_) / z, y = dotd(d, up_) / z;
    out = ImVec2(float(origin_.x + size_.x * 0.5 + fx_ * x), float(origin_.y + size_.y * 0.5 - fy_ * y));
    return true;
}

void SceneRenderer::polyline(const Dir3* pts, int n, ImU32 col, float thickness) {
    ImVec2 prev;
    bool have = false;
    for (int i = 0; i < n; ++i) {
        ImVec2 p;
        if (project(pts[i], p)) {
            if (have) dl_->AddLine(prev, p, col, thickness);
            prev = p;
            have = true;
        } else {
            have = false;
        }
    }
}

void SceneRenderer::draw_environment() {
    const ImU32 grid = theme::rgb(theme::kSceneGrid);
    const ImU32 horizon = theme::rgb(theme::kSceneHorizon);
    std::vector<Dir3> pts;
    // Latitude rings every 15°, longitude meridians every 15°: motion cues in every direction.
    for (int lat = -75; lat <= 75; lat += 15) {
        pts.clear();
        for (int yaw = 0; yaw <= 360; yaw += 2) pts.push_back(direction_from(yaw, lat));
        polyline(pts.data(), int(pts.size()), lat == 0 ? horizon : grid, lat == 0 ? 1.6f : 1.0f);
    }
    for (int yaw = 0; yaw < 360; yaw += 15) {
        pts.clear();
        for (int lat = -84; lat <= 84; lat += 2) pts.push_back(direction_from(yaw, lat));
        polyline(pts.data(), int(pts.size()), grid, 1.0f);
    }
    // Small dots at grid intersections make horizontal motion visible near the poles too.
    for (int lat = -60; lat <= 60; lat += 15)
        for (int yaw = 0; yaw < 360; yaw += 15) {
            ImVec2 p;
            if (project(direction_from(yaw + 7.5, lat + 7.5), p)) dl_->AddCircleFilled(p, 1.5f, theme::rgb(0x353C46));
        }
}

void SceneRenderer::draw_target(double yaw_deg, double pitch_deg, double radius_deg, ImU32 color, bool outline_only) {
    const Dir3 d = direction_from(yaw_deg, pitch_deg);
    ImVec2 c;
    double z;
    if (!project(d, c, &z)) return;
    // Projected angular radius (accurate near the centre of the view, adequate elsewhere).
    const float r = float(fx_ * std::tan(radius_deg * kDegToRad) / std::max(z, 0.05));
    if (outline_only) {
        dl_->AddCircle(c, r, color, 48, 1.5f);
        return;
    }
    dl_->AddCircleFilled(c, r * 1.9f, theme::rgb(theme::kTarget, 0.05f), 48);
    dl_->AddCircleFilled(c, r, color, 48);
    dl_->AddCircle(c, r, theme::rgb(0xFFFFFF, 0.6f), 48, 1.0f);
}

void SceneRenderer::draw_timer(double yaw_deg, double pitch_deg, double radius_deg, float frac_left, ImU32 color) {
    const Dir3 d = direction_from(yaw_deg, pitch_deg);
    ImVec2 c;
    double z;
    if (!project(d, c, &z) || frac_left <= 0.0f) return;
    const float r = float(fx_ * std::tan(radius_deg * kDegToRad) / std::max(z, 0.05)) + 5.0f;
    const float a0 = -0.5f * float(fit::kPi);
    dl_->PathArcTo(c, r, a0, a0 + 2.0f * float(fit::kPi) * std::min(frac_left, 1.0f), 48);
    dl_->PathStroke(color, 2.5f);
}

void SceneRenderer::draw_line(double yaw0, double pitch0, double yaw1, double pitch1, ImU32 color, float thickness) {
    ImVec2 a, b;
    if (project(direction_from(yaw0, pitch0), a) && project(direction_from(yaw1, pitch1), b)) dl_->AddLine(a, b, color, thickness);
}

void SceneRenderer::draw_crosshair(ImU32 color) {
    const ImVec2 c(origin_.x + size_.x * 0.5f, origin_.y + size_.y * 0.5f);
    const float gap = 4.0f, len = 7.0f, th = 2.0f;
    const ImU32 outline = theme::rgb(0x000000, 0.7f);
    auto bar = [&](ImVec2 a, ImVec2 b) {
        dl_->AddRectFilled(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), outline);
        dl_->AddRectFilled(a, b, color);
    };
    bar(ImVec2(c.x - gap - len, c.y - th / 2), ImVec2(c.x - gap, c.y + th / 2));
    bar(ImVec2(c.x + gap, c.y - th / 2), ImVec2(c.x + gap + len, c.y + th / 2));
    bar(ImVec2(c.x - th / 2, c.y - gap - len), ImVec2(c.x + th / 2, c.y - gap));
    bar(ImVec2(c.x - th / 2, c.y + gap), ImVec2(c.x + th / 2, c.y + gap + len));
    dl_->AddRectFilled(ImVec2(c.x - 1, c.y - 1), ImVec2(c.x + 1, c.y + 1), color);
}

}  // namespace fitapp
