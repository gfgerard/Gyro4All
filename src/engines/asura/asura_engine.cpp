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
// written too while it holds the same direction, and turned by the gyro's turn while it doesn't (to check: the stick
// fallback started at times while aiming with a scope, the game putting the look back; undone runs are logged). The player, the game's own way: the first player controller in the
// list at kControllers (items, count) holds its entity handle at +0x30; the entity handle table at kEntities maps it to
// the player. Both globals are found by the code that loads them (the same patterns resolve in both exes).
// Menus: the mouse cursor; the pause menu: the game's pause flag (a byte its player input code checks, found by that
// code). The weapon wheel (held triangle) has no flag found and the game keeps the gyro's writes while it is open:
// GyroLib's hold-to-disable activation on triangle covers it.
#include "asura.hpp"

#include "engine.hpp"
#include "log.hpp"
#include "mem.hpp"

#include <windows.h>

#include <algorithm>
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
// The copy counts as the look's while within this of the look direction (dot product of the unit vectors: about 2.5
// degrees); further away it is turned by the gyro's turn instead of overwritten.
constexpr float kSameDirection = 0.999f;
// A write moved less than this (degrees) gets no kept/undone verdict.
constexpr double kMinStep = 0.11;
// Logged at most this many times a session: undone runs, the copy's changes.
constexpr int kMaxLogs = 30;

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

double wrap180(double degrees) { return std::remainder(degrees, 360.0); }

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

    void configure() override {}

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0}}; }

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
        if (player_ != had) stored_ = false, limit_up_ = limit_down_ = kMaxPitch;
        if (player_ != had)
            log("asura: local player %s", player_ ? std::format("at {} (look direction +{:#x})", mem::module_offset(player_), kLook).c_str()
                                                  : "gone");
        read_copy(d);
        if (!player_) return f;
        std::memcpy(last_, d, sizeof d);
        const bool wrote = stored_;  // the gyro wrote the look last frame
        stored_ = false;
        if (wrote) judge_write(d);
        learn_limits(d, wrote);
        f.in_game = true;
        f.paused = paused();
        f.view_id = 1;
        f.target = "player look";
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
        pause_flag_ = data_from_code(kPauseCode, kPauseMask, sizeof kPauseCode, "the pause flag", kPauseDisp, kPauseInsnEnd);
        controllers_ = data_from_code(kControllersCode, kControllersMask, sizeof kControllersCode, "the player controllers' list");
        entities_ = data_from_code(kEntitiesCode, kEntitiesMask, sizeof kEntitiesCode, "the entity handle table");
        if (!controllers_ || !entities_) log("asura: no route to the player: no camera hook (gyro as right stick)");
    }

    // The first controller's entity, through the handle table; 0 if any step doesn't hold.
    uintptr_t find_player() const {
        uint32_t count = 0;
        const uintptr_t items = mem::rdp(controllers_ + kItems);
        if (!items || !mem::rd(controllers_ + kCount, count) || count == 0 || count > 64) return 0;
        const uintptr_t controller = mem::rdp(items);
        uint32_t handle = 0;
        if (!controller || !mem::rd(controller + kHandle, handle) || handle == 0) return 0;
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
        float c[3];
        const uintptr_t had = copy_;
        const bool had_same = copy_same_;
        copy_ = owner && mem::guarded_copy(c, owner + kOwnerLook, sizeof c) && unit(c) ? owner + kOwnerLook : 0;
        copy_same_ = copy_ && c[0] * d[0] + c[1] * d[1] + c[2] * d[2] > kSameDirection;
        if ((copy_ != had || copy_same_ != had_same) && copy_logs_ < kMaxLogs && ++copy_logs_) {
            if (!copy_) log("asura: the look copy at [player+0x588]+0x20: %s", owner ? "not a direction" : "no object");
            else log("asura: the look copy at [player+0x588]+0x20: %.1f degrees from the look (%s)", angle_between(c, d),
                     copy_same_ ? "the same: written with it" : "turned by the gyro's turn");
        }
    }

    // pre_*: the angles before the turn.
    bool store(double pitch, double yaw, double pre_pitch, double pre_yaw) {
        float d[3];
        direction_of(pitch, yaw, d);
        if (copy_) {
            // Overwritten while it holds the look's direction; else turned by the same amount (the game keeps it apart
            // from the look in some states and may start its look update from it: an untouched copy undoes the turn).
            float c[3];
            if (copy_same_) {
                mem::guarded_store(copy_, d, sizeof d);
            } else if (mem::guarded_copy(c, copy_, sizeof c) && unit(c)) {
                double cp = 0, ch = 0;
                angles_of(c, cp, ch);
                direction_of(std::clamp(cp + pitch - pre_pitch, -kMaxPitch, kMaxPitch), ch + wrap180(yaw - pre_yaw), c);
                mem::guarded_store(copy_, c, sizeof c);
            }
        }
        if (!mem::guarded_store(player_ + kLook, d, sizeof d)) return false;
        stored_pitch_ = pitch, stored_yaw_ = yaw, pre_pitch_ = pre_pitch, pre_yaw_ = pre_yaw, stored_ = true;
        return true;
    }

    // Last frame's write against the look now: kept, undone (back where it was before), or neither (the game's own turn,
    // a clamp). Runs of undone writes are logged with the copy's state, to tell what puts the look back.
    void judge_write(const float* d) {
        double p = 0, h = 0;
        angles_of(d, p, h);
        const double step = std::hypot(stored_pitch_ - pre_pitch_, wrap180(stored_yaw_ - pre_yaw_));
        if (step <= kMinStep) return;
        const double to_pre = std::hypot(p - pre_pitch_, wrap180(h - pre_yaw_));
        const double to_kept = std::hypot(p - stored_pitch_, wrap180(h - stored_yaw_));
        if (to_pre < 0.5 * step) {
            if (++undone_run_ == 3 && undone_logs_ < kMaxLogs && ++undone_logs_) {
                std::string copy = "no copy";
                float c[3];
                if (copy_ && mem::guarded_copy(c, copy_, sizeof c)) {
                    float written[3];
                    direction_of(stored_pitch_, stored_yaw_, written);
                    copy = std::format("the copy {:.2f} degrees from it and {:.2f} from the write ({})", angle_between(c, d),
                                       angle_between(c, written), copy_same_ ? "written with it" : "turned with it");
                }
                log("asura: the game undid the gyro's writes 3 frames in a row: look pitch %.2f yaw %.2f, written %.2f %.2f, "
                    "before %.2f %.2f; %s; paused flag %d",
                    p, h, stored_pitch_, stored_yaw_, pre_pitch_, pre_yaw_, copy.c_str(), int(paused_));
            }
        } else if (to_kept < 0.5 * step) {
            if (undone_run_ >= 3 && undone_logs_ <= kMaxLogs) log("asura: writes kept again after %d undone", undone_run_);
            undone_run_ = 0;
        }
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
    // A clamp, not an undone write: the yaw kept (judged on a clear yaw step only) or the pitch moved toward the write
    // before it stopped. An undone write goes back to where it was, which read as a 20-degree limit while aiming.
    void learn_limits(const float* d, bool wrote) {
        double p = 0, h = 0;
        angles_of(d, p, h);
        const double yaw_step = std::abs(wrap180(stored_yaw_ - pre_yaw_)), yaw_off = std::abs(wrap180(h - stored_yaw_));
        if (wrote && yaw_off < 0.5) {
            const bool yaw_kept = yaw_step > kMinStep && yaw_off < 0.25 * yaw_step;
            if (stored_pitch_ > p + kClamped && p > kMinLimit && p < limit_up_ - kClamped &&
                (yaw_kept || p > pre_pitch_ + kClamped))
                set_limit(limit_up_, p, "up");
            if (stored_pitch_ < p - kClamped && -p > kMinLimit && -p < limit_down_ - kClamped &&
                (yaw_kept || p < pre_pitch_ - kClamped))
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
    uintptr_t copy_ = 0;  // read_copy() this frame
    bool copy_same_ = false;
    bool stored_ = false;  // the gyro wrote the look last frame, at these angles, from these
    double stored_pitch_ = 0, stored_yaw_ = 0, pre_pitch_ = 0, pre_yaw_ = 0;
    int undone_run_ = 0, undone_logs_ = 0;  // judge_write()
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
