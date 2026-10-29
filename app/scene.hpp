// FPS-style environment for MouseFit: a yaw/pitch camera inside a reference sphere, a stationary
// crosshair and spherical targets. Drawn with ImGui's draw lists so the same code renders on every
// backend.
#pragma once

#include "imgui.h"

namespace fitapp {

struct Camera {
    double yaw_deg = 0.0;
    double pitch_deg = 0.0;
    double hfov_deg_16x9 = 103.0;  // VALORANT
};

struct Dir3 {
    double x, y, z;
};

Dir3 direction_from(double yaw_deg, double pitch_deg);
// Angle between two view directions, degrees.
double angular_distance_deg(double yaw_a, double pitch_a, double yaw_b, double pitch_b);

class SceneRenderer {
public:
    void begin(ImDrawList* dl, ImVec2 origin, ImVec2 size, const Camera& cam);
    // Projects a world direction to the screen; false if behind the camera.
    bool project(const Dir3& d, ImVec2& out, double* depth = nullptr) const;
    void draw_environment();
    void draw_target(double yaw_deg, double pitch_deg, double radius_deg, ImU32 color, bool outline_only = false);
    void draw_crosshair(ImU32 color);
    // A straight segment between two view directions (for the crosshair's trail).
    void draw_line(double yaw0, double pitch0, double yaw1, double pitch1, ImU32 color, float thickness);
    // The target's timer: a ring around it that empties clockwise as time runs out (frac_left 1 → 0).
    void draw_timer(double yaw_deg, double pitch_deg, double radius_deg, float frac_left, ImU32 color);
    double focal_px() const { return fx_; }

private:
    void polyline(const Dir3* pts, int n, ImU32 col, float thickness);
    ImDrawList* dl_ = nullptr;
    ImVec2 origin_{}, size_{};
    double fx_ = 1.0, fy_ = 1.0;
    Dir3 right_{}, up_{}, fwd_{};
};

}  // namespace fitapp
