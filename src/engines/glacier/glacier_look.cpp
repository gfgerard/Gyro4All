// 007 First Light (MODLOG 2026-10-29): the view is the active orbit camera's, and the orbit camera keeps its angles.
// The camera world ([exe+0x3E9ADA0], laid out like the Gameplay World: [world]+8 a table of component arrays {base,
// stride} indexed by entity) holds, per orbit camera, an orbit state component (+0x500, stride +0x508): radius, yaw,
// pitch (radians) and their rates (+0xC, +0x10, +0x14: the right stick's), which the orbit update (exe+0x52DEB0 loop,
// main thread, in the game loop's frame) integrates every frame, clamps to the limits component (+0x558, stride +0x560:
// radius, yaw min/max, pitch min/max) and turns into the camera's offset around the player. The active camera's handle
// is at [exe+0x3E9ADE8]+4 (its low 16 bits: the entity). So the gyro adds its turn to that yaw and pitch, on the game
// thread, from the gyro frame (before the frame's update), and the game's camera does the rest.
// Cameras that look at a point (the camera world's bitset +0x720: the aim camera of L2, but also some normal ones) take
// their orientation from that point (+0x1A50, stride +0x1A58) seen from the camera's position (the transform +0x30,
// position at +0x10; exe+0x538560): the aim camera ignores its orbit angles and keeps the point (rotating it turns the
// view 1:1), the normal ones keep the point but follow their orbit angles. So with such a camera the gyro turns both:
// each kind of camera ignores the other half. The aim ray's point is at about 1e8 m when it hits nothing.
// L2's zoom (and other cuts) runs on a short-lived transition camera without an orbit state (entity 0x11E, a new
// generation each time) that blends the orbit cameras around it, which the orbit update keeps integrating (the stick
// keeps turning them): while one is active the gyro turns every orbit camera of the update's list.
// When gameplay runs: an inline hook on the orbit update (exe+0x52DDE0, the function around its loop, from the exe's
// unwind table), called every frame while an orbit camera is updated, not in menus, loads or the pause menu (the
// player's locomotion update, used before, stops indoors). The view's angles for the status line: the derived view component of the Gameplay World (+0x88: +0x10C yaw, +0x110 pitch, radians; up negative).
#include "glacier_look.hpp"

#include "dinputpad.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "writewatch.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace gyro4all::glacier::look {

namespace {

// The player's locomotion update's start: mov rax,[rip+world] ... mov r10,[rdx+1210h]: the Gameplay World.
constexpr uint8_t kWorldCode[] = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x45, 0x33, 0xED, 0x45, 0x0F, 0xB7, 0xE0, 0x8B, 0xFA,
                                  0x41, 0x8B, 0xF4, 0x41, 0x8B, 0xD8, 0x48, 0x8B, 0x50, 0x08, 0x48, 0x8B, 0x05, 0, 0, 0,
                                  0, 0x0F, 0xB7, 0xCB, 0x48, 0x89, 0x4C, 0x24, 0x40, 0x4C, 0x8B, 0x92, 0x10, 0x12, 0x00, 0x00};
constexpr uint8_t kWorldMask[] = {0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0,
                                  0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
// The orbit update's loop (exe+0x52DE61): mov r8,[rip+camera world] ... imul r8,[rdx+508h]; imul r9,[rdx+560h];
// add r8,[rdx+500h]: the camera world, and the state and limits components it integrates and clamps with.
constexpr uint8_t kOrbitCode[] = {
    0x4C, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x8B, 0x73, 0x10, 0x48, 0x8B, 0x7B, 0x08, 0x48, 0x3B, 0xFE, 0x0F, 0x84, 0, 0, 0, 0,
    0xC5, 0xF8, 0x29, 0xB4, 0x24, 0xD0, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x29, 0xBC, 0x24, 0xC0, 0x00, 0x00, 0x00, 0xC5, 0x78,
    0x29, 0x84, 0x24, 0xB0, 0x00, 0x00, 0x00, 0xC5, 0x7A, 0x10, 0x05, 0, 0, 0, 0, 0xC5, 0x78, 0x29, 0x8C, 0x24, 0xA0, 0x00,
    0x00, 0x00, 0xC5, 0x78, 0x10, 0x0D, 0, 0, 0, 0, 0x0F, 0x1F, 0x00, 0x49, 0x8B, 0x50, 0x08, 0x48, 0x8D, 0x4C, 0x24, 0x58,
    0x0F, 0xB7, 0x1F, 0x44, 0x8B, 0xC3, 0x44, 0x8B, 0xCB, 0x4C, 0x0F, 0xAF, 0x82, 0x08, 0x05, 0x00, 0x00, 0x4C, 0x0F, 0xAF,
    0x8A, 0x60, 0x05, 0x00, 0x00, 0x4C, 0x03, 0x82, 0x00, 0x05, 0x00, 0x00};
constexpr uint8_t kOrbitMask[] = {
    0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
// The camera system copying the active camera's transform (exe+0x5404B5): mov rcx,[rip+active] ... mov ecx,[rcx+4].
constexpr uint8_t kActiveCode[] = {0x48, 0x8B, 0x0D, 0, 0, 0, 0, 0x48, 0x8B, 0xB0, 0x30, 0x03, 0x00, 0x00,
                                   0x8B, 0x05, 0, 0, 0, 0, 0x8B, 0x49, 0x04, 0x44, 0x8B, 0xC1};
constexpr uint8_t kActiveMask[] = {0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// The look-at orientation (exe+0x538574): mov rcx,[rip+camera world] ... mov r11,[rax+720h] (bitset) ... imul rcx,
// [rax+1A58h]; mov rax,[rax+1A50h] (the point).
constexpr uint8_t kLookAtCode[] = {
    0x48, 0x8B, 0x0D, 0, 0, 0, 0, 0x0F, 0xB7, 0xDA, 0x48, 0xC1, 0xEB, 0x06, 0x48, 0x8B, 0x41, 0x38, 0x4C, 0x8B, 0x98, 0x20,
    0x07, 0x00, 0x00, 0x8B, 0xC2, 0x83, 0xE0, 0x3F, 0x44, 0x0F, 0xB6, 0xD0, 0x49, 0x8B, 0x04, 0xDB, 0x4C, 0x0F, 0xA3, 0xD0,
    0x48, 0x8B, 0x9C, 0x24, 0xB0, 0x00, 0x00, 0x00, 0x0F, 0x83, 0, 0, 0, 0, 0x48, 0x8B, 0x41, 0x08, 0xC5, 0xF8, 0x29, 0xB4,
    0x24, 0x90, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x29, 0xBC, 0x24, 0x80, 0x00, 0x00, 0x00, 0xC5, 0x78, 0x29, 0x44, 0x24, 0x70,
    0xC5, 0x78, 0x29, 0x4C, 0x24, 0x60, 0xC5, 0x78, 0x29, 0x54, 0x24, 0x50, 0xC5, 0x78, 0x29, 0x5C, 0x24, 0x40, 0xC5, 0x78,
    0x29, 0x64, 0x24, 0x30, 0x0F, 0xB7, 0xCA, 0x48, 0x0F, 0xAF, 0x88, 0x58, 0x1A, 0x00, 0x00, 0x48, 0x8B, 0x80, 0x50, 0x1A,
    0x00, 0x00};
constexpr uint8_t kLookAtMask[] = {
    0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF};

constexpr uintptr_t kComponents = 0x08;  // [world]+8: the component table
constexpr uintptr_t kDerived = 0x88, kDerivedYaw = 0x10C;  // Gameplay World: the derived view (yaw, pitch)
constexpr uintptr_t kOrbit = 0x500, kLimits = 0x558;        // camera world: orbit state, its limits
constexpr uintptr_t kTransform = 0x30, kTransformPos = 0x10;  // camera world: {quat, pos} per entity
constexpr uintptr_t kLookAtPoint = 0x1A50, kBitsets = 0x38, kLookAtBits = 0x720;  // the point; [world+38h]+720h: who has one
constexpr uintptr_t kActiveHandle = 4;                       // [active]+4: the active camera's handle
constexpr double kRad = 3.14159265358979323846 / 180.0, kPi = 3.14159265358979323846;
constexpr ULONGLONG kLiveMs = 250;
// The orbit angles' senses against the gyro's (yaw right, pitch up): the yaw grows to the right, the pitch downwards.
constexpr double kYawSense = 1, kPitchSense = -1;

struct Orbit {
    float radius, yaw, pitch, radius_rate, yaw_rate, pitch_rate;
};
struct Limits {
    float radius, yaw_min, yaw_max, pitch_min, pitch_max;
};

using Update = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
Update g_update = nullptr;
uintptr_t g_world = 0, g_camera_world = 0, g_active = 0;  // the globals holding them
bool g_installed = false;

uintptr_t component(uintptr_t world_global, uintptr_t kind, uint32_t entity);

// The point a look-at camera aims at, false when the camera has none.
bool look_at_point(uint32_t handle, uintptr_t& point, uintptr_t& position) {
    const uint32_t entity = uint16_t(handle);
    const uintptr_t world = g_camera_world ? mem::rdp(g_camera_world) : 0;
    const uintptr_t bitsets = world ? mem::rdp(world + kBitsets) : 0;
    const uintptr_t bits = bitsets ? mem::rdp(bitsets + kLookAtBits) : 0;
    uint64_t word = 0;
    if (!bits || !mem::rd(bits + (entity >> 6) * 8, word) || !(word >> (entity & 63) & 1)) return false;
    point = component(g_camera_world, kLookAtPoint, handle);
    const uintptr_t transform = component(g_camera_world, kTransform, handle);
    position = transform ? transform + kTransformPos : 0;
    return point && position;
}

// What became of a look-at camera's point on a turn (the aim line counts them).
enum class Point { None, Turned, Unreadable, Near, Far };

// Turns a look-at camera's point about the camera's position: heading by -yaw (the view turns right as the point turns
// clockwise), elevation by pitch (up), within the pitch limits (radians, up negative, as the orbit's). before: the
// point as found; out: as written (Turned only).
Point turn_point(uintptr_t point, uintptr_t position, double pitch, double yaw, const Limits& limits, float (&before)[3],
                 float (&out)[3]) {
    float c[3] = {};
    if (!mem::guarded_copy(before, point, sizeof before) || !mem::guarded_copy(c, position, sizeof c)) return Point::Unreadable;
    const double dx = double(before[0]) - c[0], dy = double(before[1]) - c[1], dz = double(before[2]) - c[2];
    const double flat = std::hypot(dx, dy), dist = std::hypot(flat, dz);
    if (!std::isfinite(dist)) return Point::Unreadable;
    if (dist < 0.1) return Point::Near;
    if (dist > 1e12) return Point::Far;  // the aim ray's point at infinity is at about 1e8 m
    const double heading = std::atan2(dy, dx) - kYawSense * yaw * kRad;
    const double elevation = std::clamp(std::atan2(dz, flat) - kPitchSense * pitch * kRad, double(-limits.pitch_max),
                                        double(-limits.pitch_min));
    out[0] = float(c[0] + dist * std::cos(elevation) * std::cos(heading));
    out[1] = float(c[1] + dist * std::cos(elevation) * std::sin(heading));
    out[2] = float(c[2] + dist * std::sin(elevation));
    return mem::guarded_store(point, out, sizeof out) ? Point::Turned : Point::Unreadable;
}

// Game thread only (the gyro frame and the updates run there).
uint32_t g_camera = 0;  // the active camera's handle at the last turn
uint16_t g_logged[64] = {};  // the orbit cameras logged so far, each once (by entity: the generation changes on each switch)
ULONGLONG g_last_update = 0;
// The orbit update's argument: +8/+0x10 the begin and end of the 16-bit entities of every orbit camera, which it
// integrates every frame, active or not.
uintptr_t g_cameras = 0;
constexpr size_t kMaxCameras = 65536;  // every 16-bit entity: no list the game can make is longer

uintptr_t component(uintptr_t world_global, uintptr_t kind, uint32_t entity) {  // see the declaration above
    const uintptr_t world = world_global ? mem::rdp(world_global) : 0;
    const uintptr_t table = world ? mem::rdp(world + kComponents) : 0;
    const uintptr_t base = table ? mem::rdp(table + kind) : 0;
    uintptr_t stride = 0;
    if (!base || !mem::rd(table + kind + 8, stride) || stride == 0 || stride > 0x10000) return 0;
    return base + uintptr_t(uint16_t(entity)) * stride;
}

bool read_view(double& pitch, double& yaw) {
    const uintptr_t derived = component(g_world, kDerived, 0);
    float a[2] = {};
    if (!derived || !mem::guarded_copy(a, derived + kDerivedYaw, sizeof a) || !std::isfinite(a[0]) || !std::isfinite(a[1]) ||
        std::fabs(a[0]) > 10 || std::fabs(a[1]) > 2)
        return false;
    yaw = kYawSense * a[0] / kRad;
    pitch = kPitchSense * a[1] / kRad;
    return true;
}

// A camera's orbit state and limits, 0 when it has none (another kind of camera, or a load).
uintptr_t orbit_of(uint32_t handle, Limits& limits) {
    const uintptr_t orbit = component(g_camera_world, kOrbit, handle), lim = component(g_camera_world, kLimits, handle);
    Orbit o{};
    if (!orbit || !lim || !mem::guarded_copy(&o, orbit, sizeof o) || !mem::guarded_copy(&limits, lim, sizeof limits)) return 0;
    const bool sane = std::isfinite(o.radius) && o.radius > 0 && std::isfinite(o.yaw) && std::isfinite(o.pitch) &&
                      std::fabs(o.yaw) <= 4 && std::fabs(o.pitch) <= 2 && std::isfinite(limits.pitch_min) &&
                      std::isfinite(limits.pitch_max) && limits.pitch_min < limits.pitch_max;
    return sane ? orbit : 0;
}

// The active orbit camera's state and limits, 0 when there is none.
uintptr_t active_orbit(Limits& limits, uint32_t& handle) {
    const uintptr_t active = g_active ? mem::rdp(g_active) : 0;
    if (!active || !mem::rd(active + kActiveHandle, handle) || !handle) return 0;
    return orbit_of(handle, limits);
}

uintptr_t hooked_update(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    g_last_update = GetTickCount64();
    g_cameras = a;
    return g_update(a, b, c, d);
}

// The start of the function around code_at, from the exe's unwind table (following chained fragments), 0 if none.
uintptr_t function_start(uintptr_t code_at) {
    DWORD64 image = 0;
    const RUNTIME_FUNCTION* f = RtlLookupFunctionEntry(code_at, &image, nullptr);
    for (int depth = 0; f && depth < 8; ++depth) {
        const uint8_t* unwind = reinterpret_cast<const uint8_t*>(image + f->UnwindData);
        if (!(unwind[0] >> 3 & UNW_FLAG_CHAININFO)) return uintptr_t(image + f->BeginAddress);
        f = reinterpret_cast<const RUNTIME_FUNCTION*>(unwind + 4 + ((unwind[2] + 1) & ~1) * 2);
    }
    return 0;
}

}  // namespace

bool install() {
    uintptr_t loop = 0;
    g_world = data_from_code(kWorldCode, kWorldMask, sizeof kWorldCode, "the Gameplay World");
    g_camera_world = data_from_code(kOrbitCode, kOrbitMask, sizeof kOrbitCode, "the camera world (orbit update)", 3, 7,
                                    ".data", &loop);
    g_active = data_from_code(kActiveCode, kActiveMask, sizeof kActiveCode, "the active camera");
    if (data_from_code(kLookAtCode, kLookAtMask, sizeof kLookAtCode, "the camera world (look-at)") != g_camera_world) {
        log("glacier: this build's look-at cameras aren't known: no gyro");
        return false;
    }
    const uintptr_t update = loop ? function_start(loop) : 0;
    if (update) log("glacier: the orbit update at %s", mem::module_offset(update).c_str());
    if (!update || !g_world || !g_camera_world || !g_active) {
        log("glacier: this build's orbit camera isn't known: no gyro");
        return false;
    }
    if (!hooks::hook_function(reinterpret_cast<void*>(update), reinterpret_cast<void*>(&hooked_update),
                              reinterpret_cast<void**>(&g_update))) {
        log("glacier: the orbit update can't be hooked: no gyro");
        return false;
    }
    g_installed = true;
    writewatch::provide("orbit", [] {
        Limits l{};
        uint32_t handle = 0;
        return active_orbit(l, handle);
    });
    log("glacier: the gyro turns the active orbit camera (WriteWatch name: orbit)");
    return true;
}

bool installed() { return g_installed; }

bool live() { return g_installed && g_last_update && GetTickCount64() - g_last_update < kLiveMs; }

bool view(double& pitch, double& yaw) { return g_installed && read_view(pitch, yaw); }

namespace {

// What turn_camera did to a look-at camera's point: the outcome, the point as found and as written.
struct PointTurn {
    Point result = Point::None;
    float before[3] = {}, after[3] = {};
};

// Turns one camera: its orbit angles and, when it looks at a point, the point (each kind of camera follows one of them).
// The pitch actually turned (the orbit's, within its limits), NAN if the camera has no orbit state.
double turn_camera(uint32_t handle, uintptr_t orbit, const Limits& limits, double pitch, double yaw,
                   PointTurn* point_turn = nullptr) {
    Orbit o{};
    if (!orbit || !mem::guarded_copy(&o, orbit, sizeof o)) return NAN;
    uintptr_t point = 0, position = 0;
    PointTurn unused;
    PointTurn& pt = point_turn ? *point_turn : unused;
    pt.result = look_at_point(handle, point, position) ? turn_point(point, position, pitch, yaw, limits, pt.before, pt.after)
                                                       : Point::None;
    const float angles[2] = {
        float(std::remainder(o.yaw + kYawSense * yaw * kRad, 2 * kPi)),
        std::clamp(float(o.pitch + kPitchSense * pitch * kRad), std::min(limits.pitch_min, o.pitch), std::max(limits.pitch_max, o.pitch))};
    if (!mem::guarded_store(orbit + offsetof(Orbit, yaw), angles, sizeof angles)) return NAN;
    return (angles[1] - o.pitch) / (kPitchSense * kRad);
}

// The log line on each of the first transitions, and on any whose gyro frames didn't all turn the cameras (up to
// kFailedTransitionLines): how long, the gyro's turns during it, and the size of the orbit update's camera list.
uint32_t g_transition = 0;
ULONGLONG g_transition_at = 0;
int g_transitions = 0, g_failed_transitions = 0;
double g_transition_yaw = 0, g_transition_pitch = 0;
int g_transition_frames = 0, g_transition_turned = 0;
size_t g_transition_list = 0;  // the camera list's length at the last frame (SIZE_MAX: unreadable)
constexpr int kFailedTransitionLines = 50;

void transition_end() {
    const bool failed = g_transition_turned < g_transition_frames;
    if (g_transition && (g_transitions <= 12 || (failed && g_failed_transitions < kFailedTransitionLines))) {
        g_failed_transitions += failed;
        log("glacier: transition camera %08X for %llu ms: %d gyro frames (%d turned cameras), asked yaw %.2f pitch %.2f, "
            "%s",
            g_transition, GetTickCount64() - g_transition_at, g_transition_frames, g_transition_turned, g_transition_yaw,
            g_transition_pitch,
            g_transition_list == SIZE_MAX ? "camera list unreadable"
                                          : std::format("{} cameras in the list", g_transition_list).c_str());
    }
    g_transition = 0;
}

// A transition camera without an orbit (L2's zoom, about 0.5 s, up to 2 s seen) blends the cameras on both sides of it,
// which the stick keeps turning: the gyro turns every orbit camera, as the game re-aligns the one it returns to. The list
// grows as the level streams cameras in (64 entries were too few after a while of play: the gyro then stopped for the
// whole zoom, MODLOG 2026-11-03).
bool turn_all(double pitch, double yaw) {
    ++g_transition_frames;
    g_transition_yaw += yaw;
    g_transition_pitch += pitch;
    uintptr_t range[2] = {};
    g_transition_list = SIZE_MAX;
    if (!g_cameras || !mem::guarded_copy(range, g_cameras + 8, sizeof range) || range[1] < range[0] ||
        range[1] - range[0] > kMaxCameras * 2)
        return false;
    static uint16_t entities[kMaxCameras];  // game thread only
    const size_t n = (range[1] - range[0]) / 2;
    g_transition_list = n;
    if (!n || !mem::guarded_copy(entities, range[0], n * 2)) return false;
    bool turned = false;
    for (size_t i = 0; i < n; ++i) {
        Limits limits{};
        const uintptr_t orbit = orbit_of(entities[i], limits);
        turned |= orbit && !std::isnan(turn_camera(entities[i], orbit, limits, pitch, yaw));
    }
    g_transition_turned += turned;
    return turned;
}

// One stretch of gyro turns on one active orbit camera (an "aim" when the camera looks at a point: L2's aim cameras),
// summed up in one line when it ends: what became of the point, whether the game rewrote the one written, whether the
// view followed the turns (the derived view's movement between consecutive gyro frames against the turns asked; the
// right stick moves it too), and the game's own mouse/keyboard input meanwhile (a game that switches to its keyboard and
// mouse controls may take the aim from the mouse). Aims: every one up to kAimLines. Past that, and for cameras without
// a point, only the stretches whose view didn't follow (up to kStuckLines).
struct Aim {
    uint32_t handle = 0;
    ULONGLONG start = 0, last = 0;
    int frames = 0, points[5] = {}, rewritten = 0, pairs = 0, cursor_moves = 0;
    double asked_yaw = 0, asked_pitch = 0, view_yaw = 0, view_pitch = 0;
    bool have_view = false, wrote = false;
    double prev_yaw = 0, prev_pitch = 0, pending_yaw = 0, pending_pitch = 0;
    float written[3] = {};
    uint32_t mouse = 0, keyboard = 0;
    POINT cursor{};
};
Aim g_aim;
int g_aim_lines = 0, g_stuck_lines = 0;
constexpr int kAimLines = 40, kStuckLines = 50;
constexpr ULONGLONG kAimGapMs = 500, kPairMs = 100;  // a stretch ends after kAimGapMs without a turn
constexpr double kJudgeDeg = 2;                      // turns asked below this (in all) aren't judged,
constexpr int kJudgePairs = 10;                      // nor stretches shorter than this (the derived view lags)

void aim_end() {
    Aim& a = g_aim;
    if (!a.handle || !a.frames) {
        a = {};
        return;
    }
    const bool aim = a.frames > a.points[int(Point::None)];
    const double asked = std::hypot(a.asked_yaw, a.asked_pitch);
    const double followed = asked >= kJudgeDeg && a.pairs >= kJudgePairs
                                ? (a.view_yaw * a.asked_yaw + a.view_pitch * a.asked_pitch) / (asked * asked)
                                : NAN;
    const bool stuck = !std::isnan(followed) && followed < 0.5;
    if (stuck ? g_stuck_lines < kStuckLines : aim && g_aim_lines < kAimLines) {
        ++(stuck ? g_stuck_lines : g_aim_lines);
        uint32_t mouse = 0, keyboard = 0;
        dinputpad::desktop_input(mouse, keyboard);
        const std::string verdict = std::isnan(followed) ? std::string("too little turn to judge")
                                    : stuck ? std::format("NOT FOLLOWED ({:.0f}%)", followed * 100)
                                            : std::format("followed ({:.0f}%)", followed * 100);
        log("glacier: %s %08X for %llu ms (ended %llu ms ago): %d gyro frames; point turned %d, none %d, unreadable %d, "
            "near %d, far %d, rewritten by the game %d; asked yaw %.2f pitch %.2f, view moved yaw %.2f pitch %.2f over %d "
            "frame pairs: %s; game mouse input %u, keyboard %u, cursor moved %d",
            aim ? "aim" : "camera", a.handle, a.last - a.start, GetTickCount64() - a.last, a.frames,
            a.points[int(Point::Turned)], a.points[int(Point::None)], a.points[int(Point::Unreadable)],
            a.points[int(Point::Near)], a.points[int(Point::Far)], a.rewritten, a.asked_yaw, a.asked_pitch, a.view_yaw,
            a.view_pitch, a.pairs, verdict.c_str(), mouse - a.mouse, keyboard - a.keyboard, a.cursor_moves);
        if (stuck && g_stuck_lines == kStuckLines) log("glacier: further views that didn't follow not logged");
    }
    a = {};
}

// Before a turn of the active camera: starts or continues its stretch, and judges the last frame's turn.
void aim_frame(uint32_t handle, ULONGLONG now) {
    Aim& a = g_aim;
    if (a.handle && (a.handle != handle || now - a.last > kAimGapMs)) aim_end();
    POINT cursor{};
    GetCursorPos(&cursor);
    if (!a.handle) {
        a.handle = handle;
        a.start = now;
        dinputpad::desktop_input(a.mouse, a.keyboard);
        a.cursor = cursor;
    } else if (cursor.x != a.cursor.x || cursor.y != a.cursor.y) {
        ++a.cursor_moves;
        a.cursor = cursor;
    }
    double pitch = 0, yaw = 0;
    const bool have = read_view(pitch, yaw);
    if (have && a.have_view && a.frames && now - a.last <= kPairMs) {
        a.view_yaw += std::remainder(yaw - a.prev_yaw, 360.0);
        a.view_pitch += pitch - a.prev_pitch;
        a.asked_yaw += a.pending_yaw;
        a.asked_pitch += a.pending_pitch;
        ++a.pairs;
    }
    a.have_view = have;
    a.prev_yaw = yaw;
    a.prev_pitch = pitch;
    a.last = now;
}

// After the turn: what became of the point, and whether the game had moved the one written on the last frame.
void aim_turned(const PointTurn& pt, double pitch, double yaw) {
    Aim& a = g_aim;
    ++a.frames;
    ++a.points[int(pt.result)];
    a.pending_yaw = yaw;
    a.pending_pitch = pitch;
    if (pt.result == Point::None) {
        a.wrote = false;
        return;
    }
    if (a.wrote) {
        const double moved = std::hypot(std::hypot(double(pt.before[0]) - a.written[0], double(pt.before[1]) - a.written[1]),
                                        double(pt.before[2]) - a.written[2]);
        const double size = std::hypot(std::hypot(double(a.written[0]), double(a.written[1])), double(a.written[2]));
        a.rewritten += moved > 1e-3 * std::max(size, 1.0);
    }
    a.wrote = pt.result == Point::Turned;
    if (a.wrote) std::copy(std::begin(pt.after), std::end(pt.after), a.written);
}

}  // namespace

bool turn(double pitch, double yaw) {
    Limits limits{};
    uint32_t handle = 0;
    const uintptr_t orbit = active_orbit(limits, handle);
    Orbit o{};
    if (!orbit || !mem::guarded_copy(&o, orbit, sizeof o)) {
        aim_end();
        if (handle && handle != g_transition) {
            transition_end();
            g_transition = handle;
            g_transition_at = GetTickCount64();
            ++g_transitions;
            g_transition_frames = g_transition_turned = 0;
            g_transition_yaw = g_transition_pitch = 0;
        }
        return handle && turn_all(pitch, yaw);
    }
    transition_end();
    const uint16_t entity = uint16_t(handle);
    if (handle != g_camera && entity && std::find(std::begin(g_logged), std::end(g_logged), entity) == std::end(g_logged)) {
        const auto slot = std::find(std::begin(g_logged), std::end(g_logged), uint16_t(0));
        if (slot != std::end(g_logged)) {
            *slot = entity;
            uintptr_t point = 0, position = 0;
            log("glacier: orbit camera %08X (state %s, limits pitch %.1f..%.1f deg%s)", handle,
                mem::module_offset(orbit).c_str(), limits.pitch_min / kRad, limits.pitch_max / kRad,
                look_at_point(handle, point, position) ? ", looks at a point now" : "");
        }
    }
    g_camera = handle;
    aim_frame(handle, GetTickCount64());
    PointTurn pt;
    const bool turned = !std::isnan(turn_camera(handle, orbit, limits, pitch, yaw, &pt));
    aim_turned(pt, pitch, yaw);
    return turned;
}

std::string status() {
    if (g_aim.handle && GetTickCount64() - g_aim.last > kAimGapMs) aim_end();
    double pitch = 0, yaw = 0;
    const bool ok = view(pitch, yaw);
    return std::format("glacier: orbit camera {} handle {:08X} view {}", g_installed ? (live() ? "live" : "idle") : "not hooked",
                       g_camera, ok ? std::format("yaw {:.2f} pitch {:.2f}", yaw, pitch) : std::string("-"));
}

}  // namespace gyro4all::glacier::look
