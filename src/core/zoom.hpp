#pragma once
// Zoom compensation (GyroLib's gyro.zoom_compensation, a per-view switch in the panel): before every gl_update the core
// reports the view's field of view and the player's normal one, and GyroLib scales the gyro by
// tan(current / 2) / tan(normal / 2): a scope moves the image under the crosshair about as fast as normal play.
// Engine-neutral, no game memory access, unit-tested (tests/profile_tests.cpp).
//
// Three knobs, one fixed curve over every field of view, the same whatever was played before:
// - the view's sensitivity (panel) is the gyro's speed at the normal field of view;
// - ZoomReference= is that normal field of view, in vertical degrees (default 50.5: 80 horizontal at 16:9). Engines
//   report horizontal FOVs; the core makes them vertical with the game window's aspect ratio (vertical_fov), so a set
//   value survives a change of resolution or an ultrawide screen (games widen the horizontal FOV and keep the vertical);
// - ZoomStrength= (percent, default 100) raises the scale to strength / 100: at 100 every field of view moves the image
//   on screen as fast as the normal one, at 0 the field of view changes nothing, in between each zoom feels faster than
//   normal play by its magnification^(1 - strength / 100) (75: a 13.6x scope 1.9x, a 3.3x aim 1.35x), and wider views
//   (a sprint) speed the gyro up less.
// GyroLib's formula is fixed, so the core reports the FOV that makes it give that scale (zoom_fov).
//
// A normal FOV learnt from play (the most-held FOV, with an aim flag, settle times and hysteresis) came first and was
// dropped: it depended on what was played before (Mass Effect 3's ship and field differ in field of view) and flipped to
// the scope's FOV after a mostly scoped start (MODLOG 2026-10-30).
#include <algorithm>
#include <cmath>

namespace gyro4all {

inline constexpr double kHalfDegree = 3.14159265358979323846 / 360;  // half an angle in degrees, as radians

// The gyro's scale for a field of view and the normal one (degrees, measured the same way), raised to the strength
// (ZoomStrength / 100).
inline double zoom_scale(double fov, double normal, double strength = 1) {
    if (!(fov > 0) || !(normal > 0)) return 1;
    return std::pow(std::tan(fov * kHalfDegree) / std::tan(normal * kHalfDegree), std::max(strength, 0.0));
}

// The FOV to report with the normal one so that GyroLib's tan(fov / 2) / tan(normal / 2) is zoom_scale's (inside the
// 0..179 degrees GyroLib takes).
inline double zoom_fov(double fov, double normal, double strength) {
    const double reported = std::atan(std::tan(normal * kHalfDegree) * zoom_scale(fov, normal, strength)) / kHalfDegree;
    return std::clamp(reported, 0.1, 178.9);
}

// A horizontal FOV as the vertical one, for a screen of this aspect ratio (width / height).
inline double vertical_fov(double horizontal, double aspect = 16.0 / 9.0) {
    return std::atan(std::tan(horizontal * kHalfDegree) / aspect) / kHalfDegree;
}

// ZoomReference's default: 80 degrees horizontal at 16:9, as a vertical FOV (50.5).
inline double default_normal_fov() { return vertical_fov(80, 16.0 / 9.0); }

}  // namespace gyro4all
