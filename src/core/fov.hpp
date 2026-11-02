#pragma once
// Field-of-view values shared by the engine-neutral core and the engine modules. GyroLib's zoom compensation takes
// vertical FOV in degrees; an engine that only has horizontal FOV converts it with the camera's actual aspect ratio.
#include <cmath>
#include <numbers>

namespace gyro4all::fov {

inline bool is_valid(double degrees) {
    return std::isfinite(degrees) && degrees > 0 && degrees < 179;
}

inline double vertical_from_horizontal(double horizontal, double aspect_ratio) {
    if (!is_valid(horizontal) || !std::isfinite(aspect_ratio) || aspect_ratio <= 0) return 0;
    const double vertical = 2 * std::atan(std::tan(horizontal * std::numbers::pi / 360) / aspect_ratio) *
                            180 / std::numbers::pi;
    return is_valid(vertical) ? vertical : 0;
}

// The game window's client aspect (width / height) for the horizontal-to-vertical conversion: the largest visible
// top-level window of this process, as in notify.cpp. Looked up again at most every 500 ms (EnumWindows is too costly
// for every frame); 0 while there is no such window. Game thread.
double game_window_aspect();

}  // namespace gyro4all::fov
