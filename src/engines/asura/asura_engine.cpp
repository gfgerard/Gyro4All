// Asura (Rebellion): Zombie Army 4: Dead War (Epic, ZA4_dx12.exe and ZA4_vulkan.exe in bin\). The game is detected by its
// Asura archives: misc\common.asr in the folder above the exe's (or next to it). Denuvo-protected: no debugger was used;
// the view was traced with the core's in-process WriteWatch (MODLOG 2026-11-02). The exe's code section is the one
// executable, non-writable section (Denuvo renames them; its VM is a writable executable one).
//
// The view: the local player's look direction, `float x, y, z` (unit, y down: looking up is negative y) at player+0x5A0.
// The player's look update rebuilds pitch and yaw from it every frame, adds the mouse and stick deltas, clamps the pitch
// and writes it back; the camera manager's cameras, their matrices and every angle copy follow from it. A write there is
// kept and the view follows (write-test, user check). The pitch limit is 85 degrees both ways, less while aiming with a
// weapon (learnt from the game's clamp, so the gyro stops there instead of being undone). The update reads the current
// direction from [player+0x588]+0x20 instead when that object exists and a player virtual (+0x90) says so; that copy is
// written too while it holds the same direction (in play it was always 65-77 degrees from the look: not a copy of it).
// Every frame the controller's update (ZA4_vulkan.exe+0x9B922B, checked in play: about 165 calls a second) sets the look
// again from its camera manager's view ([controller+0x50]+0x40, a matrix), built from its current and previous cameras
// (+0x190, +0x198: type +8, vertical FOV in radians +0x18, look direction +0x34; third person type 0 at 50 degrees, the
// scope type 3 at 13.2) after pushing the look to them (exe+0x652CB0). While it blends two cameras (manager +0x14, about
// 0.25 s entering or leaving the scope: elapsed +0x18, length +0x1C) it moves the eye (+0x40) from the previous camera's
// position to the current one's; with +0x16 set it pushes nothing and aims the view from that eye at a world point
// (+0x75A0) set as the blend starts, then puts the look back to it, undoing the gyro's write (ZA4_dx12.exe+0x64D720,
// found with WriteWatch on the manager's view). Without +0x16 (also when that point is too far up or down) the view is
// the blend of the two cameras' directions. So the aim point is turned around the eye, and both cameras, with the look.
// Undone runs are logged, and with WriteWatch=on each of the first few arms a watch on player+0x5A0. The player, the
// game's own way: the first player controller in the list at kControllers (items, count) holds its entity handle at
// +0x30; the entity handle table at kEntities maps it to the player. Both globals are found by the code that loads them (the same patterns resolve in both exes).
// Menus: the mouse cursor; the pause menu: the game's pause flag (a byte its player input code checks, found by that
// code). The weapon wheel (held triangle) has no flag found and the game keeps the gyro's writes while it is open:
// GyroLib's hold-to-disable activation on triangle covers it.
// Zoom compensation: the current camera's vertical FOV (the scope's 13.2 degrees), blended like the cameras (elapsed
// and length at manager +0x18, +0x1C), against ZoomReferenceFOV= or else the third-person camera's (embedded at manager
// +0x1A0, 50 degrees by default).
#include "asura.hpp"

#include "engine.hpp"
#include "fov.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "util.hpp"
#include "writewatch.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>
#include <vector>

namespace gyro4all::asura {

namespace {

constexpr double kRad = std::numbers::pi / 180.0;
// The game's own pitch limit (the look update clamps at 1.4835 rad), smaller while aiming with a weapon (learnt:
// learn_limits). A pitch this much short of the gyro's write is a clamp; the learnt limit is dropped this far inside it.
constexpr double kMaxPitch = 85.0, kClamped = 0.05, kRelax = 10.0, kMinLimit = 20.0;

// The player controllers' list: the function that walks it (ZA4_vulkan.exe+0x69F940): push rbx; sub rsp,40h;
// lea rdx,[rip+list]; lea rcx,[rsp+20h]; call (the list's iterator).
constexpr uint8_t kControllersCode[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8D, 0x15, 0, 0, 0, 0, 0x48, 0x8D, 0x4C, 0x24, 0x20, 0xE8};
constexpr uint8_t kControllersMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
// The entity handle table: a controller's update looks its entity up (ZA4_vulkan.exe+0x9BC4D8): mov r8b,1;
// mov edx,[rcx+30h]; lea rcx,[rip+table].
constexpr uint8_t kEntitiesCode[] = {0x41, 0xB0, 0x01, 0x8B, 0x51, 0x30, 0x48, 0x8D, 0x0D, 0, 0, 0, 0};
constexpr uint8_t kEntitiesMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
constexpr size_t kDisp = 9, kInsnEnd = 13;  // both patterns
// The pause flag (1 in the pause menu): the player input update checks it (ZA4_vulkan.exe+0x9FF618): cmp [rip+flag],bl;
// movss xmm8,[rip+..]; mov [rsp+90h],r13; movaps [rsp+70h],xmm6.
constexpr uint8_t kPauseCode[] = {0x38, 0x1D, 0, 0, 0, 0, 0xF3, 0x44, 0x0F, 0x10, 0x05, 0, 0, 0, 0,
                                  0x4C, 0x89, 0xAC, 0x24, 0x90, 0x00, 0x00, 0x00, 0x0F, 0x29, 0x74, 0x24, 0x70};
constexpr uint8_t kPauseMask[] = {0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0,
                                  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr size_t kPauseDisp = 2, kPauseInsnEnd = 6;

// The controllers' list: items (pointer array) at +0, count at +0x10. A controller: its entity handle at +0x30.
constexpr uintptr_t kItems = 0x0, kCount = 0x10, kHandle = 0x30;
// The handle table: objects at +0, keys (u32 handles) at +8, capacity (a power of two) at +0x10. Open addressing: slot =
// handle & (capacity - 1), then +1, +2, +3... (the game's lookup); an empty slot (no object, key 0) ends the search.
constexpr uintptr_t kObjects = 0x0, kKeys = 0x8, kCapacity = 0x10;
constexpr uint32_t kMaxCapacity = 1u << 20;
// The player's look direction, and the object whose +0x20 copy of it the look update may read instead.
constexpr uintptr_t kLook = 0x5A0, kLookOwner = 0x588, kOwnerLook = 0x20;
// A controller's camera manager, its blend flags, its current and previous cameras; a camera's type, vertical FOV and
// look direction.
constexpr uintptr_t kManager = 0x50, kBlending = 0x14, kNoPush = 0x16, kCameras[2] = {0x190, 0x198}, kCameraType = 0x8,
                    kCameraFov = 0x18, kCameraLook = 0x34;
// The manager's blend (seconds elapsed, its length) and its embedded third-person camera. Its eye position (+0x40, the
// view matrix's origin) and the world point it keeps aimed at while blending with +0x16 set.
constexpr uintptr_t kBlendTime = 0x18, kBlendLength = 0x1C, kThirdPerson = 0x1A0, kEye = 0x40, kAimPoint = 0x75A0;
// The copy counts as the look's from within this of the look direction (dot product of the unit vectors: about 2.5
// degrees) until further than kLeaveSame (about 3.6 degrees); apart, it is left alone.
constexpr float kSameDirection = 0.999f, kLeaveSame = 0.998f;
// A write that turned the yaw less than this (degrees) gets no kept/undone verdict.
constexpr double kMinStep = 0.11;
// Logged at most this many times a session: undone runs, the copy's changes. WriteWatch requests: the first few undone
// runs, each watched this long (seconds).
constexpr int kMaxLogs = 30, kMaxWatches = 3, kWatchSeconds = 3;

// The local player, for WriteWatch=player+<hex offset> (read on the watcher thread).
std::atomic<uintptr_t> g_player{0};

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

// The exe's whole image.
mem::Range image_range() {
    const uintptr_t base = exe_base();
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    return {base, base + nt->OptionalHeader.SizeOfImage};
}

// The exe address a RIP-relative instruction refers to, from the code around it. 0 unless the pattern is found and all
// its matches agree.
uintptr_t data_from_code(const uint8_t* code, const uint8_t* mask, size_t n, const char* what, size_t disp_at = kDisp,
                         size_t insn_end = kInsnEnd) {
    const mem::Range image = image_range();
    uintptr_t found = 0;
    int hits = 0;
    for (const uintptr_t at : mem::find_pattern(mem::code_sections(exe_base(), true), code, mask, n)) {
        int32_t disp = 0;
        if (!mem::rd(at + disp_at, disp)) continue;
        const uintptr_t target = at + insn_end + disp;
        if (target >= image.begin && target < image.end && target != found) found = target, ++hits;
    }
    if (hits != 1) {
        log("asura: the code that refers to %s matched %d different addresses; not used", what, hits);
        return 0;
    }
    log("asura: %s at %s (found by its code)", what, mem::module_offset(found).c_str());
    return found;
}

bool cursor_shown() {
    CURSORINFO ci{sizeof ci};
    return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor;
}

// Degrees: heading (yaw, growing to the right) and pitch (up) of a look direction.
void angles_of(const float* d, double& pitch, double& yaw) {
    pitch = -std::asin(std::clamp(double(d[1]), -1.0, 1.0)) / kRad;
    yaw = std::atan2(double(d[0]), double(d[2])) / kRad;
}

// The look direction of these angles (degrees).
void direction_of(double pitch, double yaw, float* d) {
    const double p = pitch * kRad, h = yaw * kRad;
    d[0] = float(std::cos(p) * std::sin(h)), d[1] = float(-std::sin(p)), d[2] = float(std::cos(p) * std::cos(h));
}

bool unit(const float* d) {
    if (!std::isfinite(d[0]) || !std::isfinite(d[1]) || !std::isfinite(d[2])) return false;
    const float n = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    return n > 0.98f && n < 1.02f;
}

// Degrees between two unit directions.
double angle_between(const float* a, const float* b) {
    return std::acos(std::clamp(double(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]), -1.0, 1.0)) / kRad;
}

class AsuraEngine final : public Engine {
public:
    const char* name() const override { return "Asura"; }

    // Asura's archives: misc\common.asr above the exe's folder (bin\), or next to it.
    bool detect(const std::wstring& exe_path) const override {
        const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
        const std::wstring parent = dir.substr(0, dir.find_last_of(L"\\/"));
        return exists(parent + L"\\misc\\common.asr") || exists(dir + L"\\misc\\common.asr");
    }

    void configure() override { load_zoom_reference(); }
    void settings_changed() override { load_zoom_reference(); }

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0, true}}; }

    // The default view while the local player's look direction can be read (else loading or the front end). Without the
    // globals at all: the gyro as right stick.
    EngineFrame read_frame() override {
        if (!searched_) find_globals();
        EngineFrame f;
        const bool was = cursor_;
        cursor_ = cursor_shown();
        if (cursor_ != was) log("asura: mouse cursor %s", cursor_ ? "shown (menu)" : "hidden");
        f.cursor = cursor_;
        if (!controllers_ || !entities_) {
            f.no_camera = true;
            f.in_game = true;
            f.view_id = 1;
            f.target = "no camera hook (gyro as right stick)";
            return f;
        }
        const uintptr_t had = player_;
        float d[3];
        player_ = find_player();
        if (player_ && !look(d)) player_ = 0;
        if (player_ != had) stored_ = false, limit_up_ = limit_down_ = kMaxPitch, g_player = player_;
        if (player_ != had)
            log("asura: local player %s", player_ ? std::format("at {} (look direction +{:#x})", mem::module_offset(player_), kLook).c_str()
                                                  : "gone");
        read_copy(d);
        read_cameras(d);
        if (!player_) return f;
        std::memcpy(last_, d, sizeof d);
        const bool wrote = stored_;  // the gyro wrote the look last frame
        stored_ = false;
        verdict_ = Verdict::None;
        if (wrote) judge_write(d);
        else undone_run_ = 0;
        learn_limits(d, wrote);
        f.in_game = true;
        f.paused = paused();
        f.view_id = 1;
        f.target = "player look";
        if (auto z = zoom_fov()) f.zoom_fov = *z;
        return f;
    }

    // The pitch the view has left before the game's limit, in the gyro's sense (up positive, degrees).
    void limit_turn(double& pitch, double& /*yaw*/) override {
        float d[3];
        if (!look(d)) return;
        double up = 0, yaw = 0;
        angles_of(d, up, yaw);
        pitch = std::clamp(pitch, std::min(0.0, -limit_down_ - up), std::max(0.0, limit_up_ - up));
    }

    // Gyro senses (pitch up, yaw right, degrees): the direction rebuilt from its turned angles, still a unit vector.
    bool apply(double pitch, double yaw) override {
        float d[3];
        if (!look(d)) return false;
        double p = 0, h = 0;
        angles_of(d, p, h);
        return store(std::clamp(p + pitch, std::min(p, -limit_down_), std::max(p, limit_up_)), h + yaw, p, h);
    }

    Recenter recenter_support(std::string& detail) override {
        float d[3];
        if (!look(d)) {
            detail = "look direction not readable";
            return Recenter::NotYet;
        }
        double p = 0, h = 0;
        angles_of(d, p, h);
        detail = std::format("pitch {:.1f}", p);
        return Recenter::Yes;
    }

    bool recenter(double fraction) override {
        float d[3];
        if (!look(d)) return false;
        double p = 0, h = 0;
        angles_of(d, p, h);
        return store(p * (1.0 - fraction), h, p, h);
    }

    bool view_angles(double& pitch, double& yaw, uintptr_t& camera) override {
        float d[3];
        if (!look(d)) return false;
        angles_of(d, pitch, yaw);
        camera = player_;
        return true;
    }

    bool sees_menus() const override { return false; }

    std::string status_scene() const override {
        if (!controllers_ || !entities_) return std::format("asura: no camera hook (gyro as right stick), cursor={}", int(cursor_));
        if (!player_) return std::format("asura: no local player (menu or loading), cursor={}", int(cursor_));
        double p = 0, h = 0;
        angles_of(last_, p, h);
        return std::format("asura: player look pitch {:.1f} yaw {:.1f} (deg), cursor={}", p, h, int(cursor_));
    }

private:
    void find_globals() {
        searched_ = true;
        writewatch::provide("player", [] { return g_player.load(); });
        pause_flag_ = data_from_code(kPauseCode, kPauseMask, sizeof kPauseCode, "the pause flag", kPauseDisp, kPauseInsnEnd);
        controllers_ = data_from_code(kControllersCode, kControllersMask, sizeof kControllersCode, "the player controllers' list");
        entities_ = data_from_code(kEntitiesCode, kEntitiesMask, sizeof kEntitiesCode, "the entity handle table");
        if (!controllers_ || !entities_) log("asura: no route to the player: no camera hook (gyro as right stick)");
    }

    // The first controller's entity, through the handle table; 0 if any step doesn't hold.
    uintptr_t find_player() {
        controller_ = 0;
        uint32_t count = 0;
        const uintptr_t items = mem::rdp(controllers_ + kItems);
        if (!items || !mem::rd(controllers_ + kCount, count) || count == 0 || count > 64) return 0;
        const uintptr_t controller = mem::rdp(items);
        uint32_t handle = 0;
        if (!controller || !mem::rd(controller + kHandle, handle) || handle == 0) return 0;
        controller_ = controller;
        const uintptr_t objects = mem::rdp(entities_ + kObjects), keys = mem::rdp(entities_ + kKeys, 4);
        uint32_t capacity = 0;
        if (!objects || !keys || !mem::rd(entities_ + kCapacity, capacity) || capacity == 0 || capacity > kMaxCapacity ||
            (capacity & (capacity - 1)))
            return 0;
        const uint32_t mask = capacity - 1;
        uint32_t slot = handle & mask;
        for (uint32_t step = 1; step <= capacity; slot = (slot + step++) & mask) {
            uintptr_t object = 0;
            uint32_t key = 0;
            if (!mem::rd(objects + 8 * uintptr_t(slot), object) || !mem::rd(keys + 4 * uintptr_t(slot), key)) return 0;
            if (object && key == handle) return mem::plausible(object) ? object : 0;
            if (!object && key == 0) return 0;
        }
        return 0;
    }

    // The player's look direction, if readable and a unit vector.
    bool look(float* d) const { return player_ && mem::guarded_copy(d, player_ + kLook, 3 * sizeof(float)) && unit(d); }

    // The direction at [player+0x588]+0x20, which the look update starts from instead of the look in some states. copy_:
    // its address while it is a unit vector; copy_same_: within kSameDirection of the look d. Its changes are logged.
    void read_copy(const float* d) {
        const uintptr_t owner = player_ ? mem::rdp(player_ + kLookOwner) : 0;
        float* const c = copy_dir_;
        const uintptr_t had = copy_;
        const bool had_same = copy_same_;
        copy_ = owner && mem::guarded_copy(c, owner + kOwnerLook, sizeof copy_dir_) && unit(c) ? owner + kOwnerLook : 0;
        copy_same_ = copy_ && c[0] * d[0] + c[1] * d[1] + c[2] * d[2] > (copy_ == had && had_same ? kLeaveSame : kSameDirection);
        if ((copy_ != had || copy_same_ != had_same) && copy_logs_ < kMaxLogs && ++copy_logs_) {
            if (!copy_) log("asura: the look copy at [player+0x588]+0x20: %s", owner ? "not a direction" : "no object");
            else log("asura: the look copy at [player+0x588]+0x20: %.1f degrees from the look (%s)", angle_between(c, d),
                     copy_same_ ? "the same: written with it" : "left alone");
        }
    }

    // The camera manager's current and previous cameras (cameras_: 0 where none, or not a direction). Their changes
    // are logged (with the blend flags).
    void read_cameras(const float* d) {
        const uintptr_t manager = player_ && controller_ ? mem::rdp(controller_ + kManager) : 0;
        uintptr_t now[2] = {};
        float c[3];
        for (int i = 0; i < 2; ++i) {
            const uintptr_t camera = manager ? mem::rdp(manager + kCameras[i]) : 0;
            if (camera && mem::guarded_copy(c, camera + kCameraLook, sizeof c) && unit(c)) now[i] = camera;
        }
        manager_ = now[0] ? manager : 0;
        uint8_t blending = 0, no_push = 0;
        if (manager) mem::rd(manager + kBlending, blending), mem::rd(manager + kNoPush, no_push);
        if ((now[0] != cameras_[0] || now[1] != cameras_[1]) && camera_logs_ < kMaxLogs && ++camera_logs_)
            log("asura: cameras: current %s, previous %s; blending %d, look pushed %s", describe_camera(now[0], d).c_str(),
                describe_camera(now[1], d).c_str(), int(blending), no_push ? "only outside blends" : "always");
        cameras_[0] = now[0], cameras_[1] = now[1];
    }

    // [Gyro4All] ZoomReferenceFOV= (horizontal degrees, profile or Gyro4All.ini); 0 = the third-person camera's.
    void load_zoom_reference() {
        const double fov = fov::reference_setting();
        if (fov != zoom_reference_)
            log("asura: zoom reference FOV %.1f degrees ([Gyro4All] ZoomReferenceFOV%s)", fov,
                fov > 0 ? "" : ": 0, the third-person camera's");
        zoom_reference_ = fov;
    }

    // A camera's vertical FOV in degrees, 0 if not readable or not a FOV.
    static double camera_fov(uintptr_t camera) {
        float radians = 0;
        if (!camera || !mem::rd(camera + kCameraFov, radians)) return 0;
        const double degrees = radians / kRad;
        return fov::is_valid(degrees) ? degrees : 0;
    }

    // The current and unzoomed vertical FOV for GyroLib's zoom compensation: the current camera's, blended from the
    // previous one's during a blend; the unzoomed one ZoomReferenceFOV= (converted with the window's aspect) or the
    // third-person camera's. Changes of the current camera's are logged.
    std::optional<ZoomFov> zoom_fov() {
        if (!manager_) return std::nullopt;
        double current = camera_fov(cameras_[0]);
        if (!current) return std::nullopt;
        const double target = current;
        uint8_t blending = 0;
        float elapsed = 0, length = 0;
        if (cameras_[1] && cameras_[1] != cameras_[0] && mem::rd(manager_ + kBlending, blending) && blending &&
            mem::rd(manager_ + kBlendTime, elapsed) && mem::rd(manager_ + kBlendLength, length) && length > 0) {
            if (const double from = camera_fov(cameras_[1]))
                current = from + (current - from) * std::clamp(double(elapsed) / length, 0.0, 1.0);
        }
        double reference = zoom_reference_ > 0 ? fov::vertical_from_horizontal(zoom_reference_, fov::game_window_aspect())
                                               : camera_fov(manager_ + kThirdPerson);
        if (!fov::is_valid(reference)) return std::nullopt;
        if (std::abs(target - logged_fov_) > 0.05 && fov_logs_ < kMaxLogs && ++fov_logs_) {
            log("asura: camera FOV %.1f degrees vertical (unzoomed %.1f)", target, reference);
            logged_fov_ = target;
        }
        return ZoomFov{current, reference};
    }

    std::string describe_camera(uintptr_t camera, const float* d) const {
        if (!camera) return "none";
        uint32_t type = 0;
        float fov = 0, c[3];
        mem::rd(camera + kCameraType, type);
        mem::rd(camera + kCameraFov, fov);
        const bool read = mem::guarded_copy(c, camera + kCameraLook, sizeof c);
        return std::format("{} (type {}, vertical FOV {:.1f}, {:.2f} degrees from the look)", mem::module_offset(camera), type,
                           fov / kRad, read ? angle_between(c, d) : -1.0);
    }

    // While the manager blends with +0x16 set, the view looks from the moving eye at a fixed world point (the aim point,
    // set as the blend starts) and the look is put back to that: the point is turned around the eye with the gyro.
    void turn_aim_point(double pitch, double yaw) {
        uint8_t blending = 0, aimed = 0;
        float eye[3], point[3];
        if (!manager_ || !mem::rd(manager_ + kBlending, blending) || !blending || !mem::rd(manager_ + kNoPush, aimed) ||
            !aimed || !mem::guarded_copy(eye, manager_ + kEye, sizeof eye) ||
            !mem::guarded_copy(point, manager_ + kAimPoint, sizeof point))
            return;
        float v[3] = {point[0] - eye[0], point[1] - eye[1], point[2] - eye[2]};
        const double length = std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
        if (!std::isfinite(length) || length < 0.01 || length > 1e6) return;
        for (float& x : v) x = float(x / length);
        double p = 0, h = 0;
        angles_of(v, p, h);
        direction_of(std::clamp(p + pitch, -kMaxPitch, kMaxPitch), h + wrap180(yaw), v);
        for (int i = 0; i < 3; ++i) point[i] = eye[i] + float(v[i] * length);
        if (mem::guarded_store(manager_ + kAimPoint, point, sizeof point) && aim_logs_ < kMaxLogs && ++aim_logs_)
            log("asura: blending: the aim point %.1f away turned with the gyro", length);
    }

    // pre_*: the angles before the turn.
    bool store(double pitch, double yaw, double pre_pitch, double pre_yaw) {
        float d[3];
        direction_of(pitch, yaw, d);
        // The cameras, turned by the same amount: while the manager blends them it doesn't push the look to them and puts
        // the look back to their directions; outside blends they are overwritten with the look anyway.
        for (int i = 0; i < 2; ++i) {
            const uintptr_t camera = cameras_[i];
            float c[3];
            if (!camera || (i == 1 && camera == cameras_[0]) || !mem::guarded_copy(c, camera + kCameraLook, sizeof c) || !unit(c))
                continue;
            double cp = 0, ch = 0;
            angles_of(c, cp, ch);
            direction_of(std::clamp(cp + pitch - pre_pitch, -kMaxPitch, kMaxPitch), ch + wrap180(yaw - pre_yaw), c);
            mem::guarded_store(camera + kCameraLook, c, sizeof c);
        }
        turn_aim_point(pitch - pre_pitch, yaw - pre_yaw);
        // The copy, overwritten while it holds the look's direction (else it is something else: left alone).
        if (copy_ && copy_same_ && mem::guarded_store(copy_, d, sizeof d)) std::memcpy(copy_dir_, d, sizeof d);
        if (!mem::guarded_store(player_ + kLook, d, sizeof d)) return false;
        // A second write in the frame (a recenter after the gyro's turn) keeps the first one's starting angles.
        if (!stored_) pre_pitch_ = pre_pitch, pre_yaw_ = pre_yaw;
        stored_pitch_ = pitch, stored_yaw_ = yaw, stored_ = true;
        return true;
    }

    // Last frame's write against the look now (verdict_): kept, undone (back where it was before), or neither (the game's
    // own turn). Judged on yaw alone, like the core's stick fallback: the pitch limit stops a pitch write as if it were
    // undone. Runs of undone writes are logged with the copy's state, to tell what puts the look back.
    void judge_write(const float* d) {
        double p = 0, h = 0;
        angles_of(d, p, h);
        const double step = std::abs(wrap180(stored_yaw_ - pre_yaw_));
        const double to_pre = std::abs(wrap180(h - pre_yaw_)), to_kept = std::abs(wrap180(h - stored_yaw_));
        if (step > kMinStep && to_pre < 0.5 * step) verdict_ = Verdict::Undone;
        else if (step > kMinStep && to_kept < 0.5 * step) verdict_ = Verdict::Kept;
        if (verdict_ == Verdict::Undone) {
            if (++undone_run_ == 3 && watches_ < kMaxWatches) {
                ++watches_;
                writewatch::request(std::format("player+{:x}", kLook).c_str(), kWatchSeconds, "the game undid the gyro's writes");
            }
            if (undone_run_ == 3 && undone_logs_ < kMaxLogs) {
                ++undone_logs_;
                run_logged_ = true;
                std::string copy = "no copy";
                if (copy_) {
                    float written[3];
                    direction_of(stored_pitch_, stored_yaw_, written);
                    double cp = 0, ch = 0;
                    angles_of(copy_dir_, cp, ch);
                    copy = std::format("the copy (pitch {:.2f} yaw {:.2f}) {:.2f} degrees from it and {:.2f} from the write ({})",
                                       cp, ch, angle_between(copy_dir_, d), angle_between(copy_dir_, written),
                                       copy_same_ ? "written with it" : "left alone");
                }
                log("asura: the game undid the gyro's writes 3 frames in a row: look pitch %.2f yaw %.2f, written %.2f %.2f, "
                    "before %.2f %.2f; %s; camera %s, previous %s; paused flag %d",
                    p, h, stored_pitch_, stored_yaw_, pre_pitch_, pre_yaw_, copy.c_str(), describe_camera(cameras_[0], d).c_str(),
                    describe_camera(cameras_[1], d).c_str(), int(paused_));
            }
            return;
        }
        // A run is undone writes in consecutive frames only.
        if (verdict_ == Verdict::Kept && run_logged_) log("asura: writes kept again after %d undone", undone_run_);
        undone_run_ = 0;
        run_logged_ = false;
    }

    // The pause flag is set (not found or not readable: not paused).
    bool paused() {
        uint8_t flag = 0;
        const bool was = paused_;
        paused_ = pause_flag_ && mem::rd(pause_flag_, flag) && flag == 1;
        if (paused_ != was) log("asura: %s", paused_ ? "paused" : "unpaused");
        return paused_;
    }

    // The game's pitch limit is smaller while aiming with a weapon: a write past it comes back clamped, and that pitch is
    // the limit that way from then on. The full limit again once the view is kRelax inside it, or past it (the mouse).
    // A clamp, not an undone write (judge_write's verdict): the yaw kept, or the pitch moved a good part of the way toward
    // the write before it stopped (not a nudge from the game's aim assist). An undone write goes back to where it was,
    // which read as a 20-degree limit while aiming.
    void learn_limits(const float* d, bool wrote) {
        double p = 0, h = 0;
        angles_of(d, p, h);
        if (wrote && verdict_ != Verdict::Undone && std::abs(wrap180(h - stored_yaw_)) < 0.5) {
            const bool yaw_kept = verdict_ == Verdict::Kept;
            const double moved = std::max(kClamped, 0.25 * std::abs(stored_pitch_ - pre_pitch_));
            if (stored_pitch_ > p + kClamped && p > kMinLimit && p < limit_up_ - kClamped && (yaw_kept || p > pre_pitch_ + moved))
                set_limit(limit_up_, p, "up");
            if (stored_pitch_ < p - kClamped && -p > kMinLimit && -p < limit_down_ - kClamped &&
                (yaw_kept || p < pre_pitch_ - moved))
                set_limit(limit_down_, -p, "down");
        }
        if (limit_up_ < kMaxPitch && (p > limit_up_ + kClamped || p < limit_up_ - kRelax)) set_limit(limit_up_, kMaxPitch, "up");
        if (limit_down_ < kMaxPitch && (-p > limit_down_ + kClamped || -p < limit_down_ - kRelax))
            set_limit(limit_down_, kMaxPitch, "down");
    }

    void set_limit(double& limit, double value, const char* way) {
        limit = std::min(value, kMaxPitch);
        log("asura: pitch limit %s %.1f degrees%s", way, limit, limit < kMaxPitch ? " (the game clamped the gyro's turn: aiming?)" : "");
    }

    bool searched_ = false;
    uintptr_t controllers_ = 0, entities_ = 0;
    uintptr_t player_ = 0;
    uintptr_t controller_ = 0;   // find_player()
    uintptr_t manager_ = 0;      // read_cameras(): the camera manager while it has a current camera
    double zoom_reference_ = 0;  // [Gyro4All] ZoomReferenceFOV=, horizontal degrees; 0 = the third-person camera's
    double logged_fov_ = 0;
    int fov_logs_ = 0;
    uintptr_t cameras_[2] = {};  // read_cameras(): the manager's current and previous cameras
    int aim_logs_ = 0;           // turn_aim_point()
    int camera_logs_ = 0;
    uintptr_t copy_ = 0;  // read_copy() this frame, its direction (kept up to date by store())
    float copy_dir_[3] = {0, 0, 1};
    bool copy_same_ = false;
    bool stored_ = false;  // the gyro wrote the look last frame, at these angles, from these
    double stored_pitch_ = 0, stored_yaw_ = 0, pre_pitch_ = 0, pre_yaw_ = 0;
    enum class Verdict { None, Kept, Undone } verdict_ = Verdict::None;  // judge_write(), this frame
    int undone_run_ = 0, undone_logs_ = 0, watches_ = 0;
    bool run_logged_ = false;  // this run's start was logged
    double limit_up_ = kMaxPitch, limit_down_ = kMaxPitch;  // learn_limits()
    uintptr_t pause_flag_ = 0;
    bool paused_ = false;
    int copy_logs_ = 0;
    float last_[3] = {0, 0, 1};
    bool cursor_ = false;
};

}  // namespace

Engine& engine() {
    static AsuraEngine instance;
    return instance;
}

}  // namespace gyro4all::asura
