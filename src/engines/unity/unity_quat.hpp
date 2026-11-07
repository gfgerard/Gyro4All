#pragma once
#include <algorithm>
#include <cmath>
#include <numbers>

namespace gyro4all::unity {

struct Quaternion {
    double x = 0, y = 0, z = 0, w = 1;
};

struct ViewAngles {
    double pitch = 0;  // up, degrees
    double yaw = 0;    // right, degrees
};

// UnityEngine.Quaternion as the runtime hands it over.
struct NativeQuaternion {
    float x, y, z, w;
};

inline Quaternion from_native(const NativeQuaternion& q) { return {q.x, q.y, q.z, q.w}; }
inline NativeQuaternion to_native(const Quaternion& q) { return {float(q.x), float(q.y), float(q.z), float(q.w)}; }

inline Quaternion normalize(Quaternion q) {
    const double length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!std::isfinite(length) || length < 1e-12) return {};
    q.x /= length, q.y /= length, q.z /= length, q.w /= length;
    return q;
}

inline Quaternion multiply(const Quaternion& a, const Quaternion& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// Unity's ZXY order, with positive X pitching down and positive Y yawing right.
inline Quaternion from_euler(double pitch, double yaw, double roll = 0) {
    constexpr double to_rad = std::numbers::pi / 180.0;
    const double hx = pitch * to_rad / 2, hy = yaw * to_rad / 2, hz = roll * to_rad / 2;
    const double sx = std::sin(hx), cx = std::cos(hx);
    const double sy = std::sin(hy), cy = std::cos(hy);
    const double sz = std::sin(hz), cz = std::cos(hz);
    return normalize({sx * cy * cz + cx * sy * sz,
                      cx * sy * cz - sx * cy * sz,
                      cx * cy * sz - sx * sy * cz,
                      cx * cy * cz + sx * sy * sz});
}

inline bool finite(const Quaternion& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

inline ViewAngles view_angles(const Quaternion& value) {
    const Quaternion q = normalize(value);
    const double fx = 2 * (q.x * q.z + q.w * q.y);
    const double fy = 2 * (q.y * q.z - q.w * q.x);
    const double fz = 1 - 2 * (q.x * q.x + q.y * q.y);
    constexpr double to_deg = 180.0 / std::numbers::pi;
    return {std::asin(std::clamp(fy, -1.0, 1.0)) * to_deg, std::atan2(fx, fz) * to_deg};
}

// World yaw, then local pitch. This keeps the camera's existing roll and doesn't depend on Euler read-back.
inline Quaternion add_turn(const Quaternion& rotation, double pitch_up, double yaw_right) {
    if (!finite(rotation) || !std::isfinite(pitch_up) || !std::isfinite(yaw_right)) return rotation;
    return normalize(multiply(multiply(from_euler(0, yaw_right), rotation), from_euler(-pitch_up, 0)));
}

inline Quaternion level(const Quaternion& rotation) {
    return from_euler(0, view_angles(rotation).yaw);
}

}  // namespace gyro4all::unity
