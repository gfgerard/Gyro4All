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

// The up of a body's yaw: that of the frame it hangs in (its parent's rotation), or its own when it has no parent. A body
// hanging from an upright parent may carry the look's pitch too (PowerWash Simulator's HeadTurn): its own up would be
// tilted. A root body on a planet's side (Outer Wilds' Player_Body) has only its own up; the world's would tilt it.
struct Vector3 {
    double x = 0, y = 1, z = 0;
};

inline Vector3 up_of(const Quaternion& frame) {
    const Quaternion q = normalize(frame);
    return {2 * (q.x * q.y - q.w * q.z), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z + q.w * q.x)};
}

inline Vector3 right_of(const Quaternion& frame) {
    const Quaternion q = normalize(frame);
    return {1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y + q.w * q.z), 2 * (q.x * q.z - q.w * q.y)};
}

// Which up a body yaws around (see above). The parent's only when the body leans from it by a pitch alone, its right
// level in the parent's frame (a head carrying the look's pitch); a body hanging from an unrelated frame keeps its own.
inline bool yaw_up_from_parent(const Quaternion& own, const Quaternion& parent) {
    const Vector3 right = right_of(own), up = up_of(parent);
    return std::abs(right.x * up.x + right.y * up.y + right.z * up.z) < 0.1;
}

// A turn of yaw_right degrees around up (a unit vector in world space): positive turns right, as Unity's yaw does.
inline Quaternion yaw_about(const Vector3& up, double yaw_right) {
    const double h = yaw_right * std::numbers::pi / 180.0 / 2, s = std::sin(h);
    return normalize({up.x * s, up.y * s, up.z * s, std::cos(h)});
}

inline Quaternion add_yaw_about(const Quaternion& rotation, const Vector3& up, double yaw_right) {
    if (!finite(rotation) || !std::isfinite(yaw_right)) return rotation;
    return normalize(multiply(yaw_about(up, yaw_right), rotation));
}

// How far a body yawed around up from one rotation to the next (degrees, right): the twist about up of the change, so a
// tilt or roll adds nothing.
inline double yaw_between_about(const Quaternion& before, const Quaternion& after, const Vector3& up) {
    const Quaternion a = normalize(before), b = normalize(after);
    Quaternion delta = multiply(b, {-a.x, -a.y, -a.z, a.w});
    if (!finite(delta)) return 0;
    if (delta.w < 0) delta = {-delta.x, -delta.y, -delta.z, -delta.w};  // the short way round
    const double along = delta.x * up.x + delta.y * up.y + delta.z * up.z;
    return 2 * std::atan2(along, delta.w) * 180.0 / std::numbers::pi;
}

// A rotation's view angles in a body's frame: its pitch and yaw relative to the body, the yaw plus body_yaw, the body's own
// turns around its up added up (yaw_between_about). For an upright body (a yaw alone, body_yaw its world yaw) the same as
// view_angles; for one on a planet's side or in a turning frame (Outer Wilds), what the player's look turned, without the
// world's drift and tilt. The yaw isn't wrapped.
inline ViewAngles angles_in_body(const Quaternion& body, double body_yaw, const Quaternion& rotation) {
    const Quaternion b = normalize(body);
    ViewAngles angles = view_angles(multiply({-b.x, -b.y, -b.z, b.w}, rotation));
    angles.yaw += body_yaw;
    return angles;
}

inline Quaternion level(const Quaternion& rotation) {
    return from_euler(0, view_angles(rotation).yaw);
}

}  // namespace gyro4all::unity
