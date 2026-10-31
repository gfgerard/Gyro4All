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

}  // namespace gyro4all::fov
