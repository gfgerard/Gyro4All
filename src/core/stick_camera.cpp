#include "stick_camera.hpp"

#include <algorithm>
#include <cmath>

namespace gyro4all {

void StickCamera::configure(const Settings& s) {
    s_ = s;
    s_.speed = std::max(0.0, s_.speed);
    s_.speed_pitch = std::max(0.0, s_.speed_pitch);
    s_.deadzone = std::clamp(s_.deadzone, 0.0, 0.95);
    s_.curve = std::clamp(s_.curve, 0.1, 10.0);
    s_.edge = std::clamp(s_.edge, s_.deadzone, 1.0);
    s_.edge_speed = std::max(0.0, s_.edge_speed);
    s_.edge_ramp_ms = std::max(0.0, s_.edge_ramp_ms);
    reset();
}

void StickCamera::reset() {
    last_ns_ = 0;
    edge_s_ = 0;
}

void StickCamera::turn(float x, float y, uint64_t now_ns, double& pitch, double& yaw) {
    pitch = yaw = 0;
    const double dt = last_ns_ && now_ns > last_ns_ ? std::min(double(now_ns - last_ns_) * 1e-9, kMaxDt) : 0;
    last_ns_ = now_ns;
    const double length = std::hypot(double(x), double(y));
    const double magnitude = std::min(length, 1.0);  // a square gate reaches past 1 on the diagonals
    if (magnitude <= s_.deadzone) {
        edge_s_ = 0;
        return;
    }
    if (magnitude >= s_.edge) edge_s_ += dt;
    else edge_s_ = 0;
    const double r = std::pow((magnitude - s_.deadzone) / (1 - s_.deadzone), s_.curve);
    double factor = 1;
    if (edge_s_ > 0 && s_.speed > 0) {
        const double ramp = s_.edge_ramp_ms > 0 ? std::min(edge_s_ * 1000 / s_.edge_ramp_ms, 1.0) : 1.0;
        factor = 1 + (s_.edge_speed / s_.speed - 1) * ramp;
    }
    const double step = r * factor * dt;
    yaw = x / length * s_.speed * step;
    pitch = y / length * s_.speed_pitch * step * (s_.invert_y ? -1 : 1);
}

}  // namespace gyro4all
