// Genome (Piranha Bytes): ELEX (MODLOG 2026-11-24). Native x64 (the 32-bit Risen games can't load this DLL), D3D11,
// MSVC RTTI kept, the engine linked into the exe (<root>\system\ELEX.exe), data in <root>\data\packed. The exe imports
// VERSION.dll (not winmm.dll): install Gyro4All as version.dll. Controllers through XInput 1.3 by ordinal: the sticks
// with XInputGetState, every button with XInputGetKeystroke (the core translates both for a DualSense).
//
// The camera (found with camtrace in the 2018 Steam build, exe time stamp 1521487194; write tests and hardware
// breakpoints): a static gCCameraAdmin in the exe's .data runs the camera once per frame on the main thread (its vtable
// slot 37, called by its CProcessor). +0x1C0 is the active camera's name (a Genome string, char*): "cam_normal" in third
// person, "cam_ranged"/"cam_ranged_dist" aiming ranged weapons. Its camera state (+0x330) holds:
//   +0x39C  yaw offset, float degrees, added to the base frame (the game keeps it at 0; not written)
//   +0x3A0  pitch, float degrees, + looks down: the source of the view's pitch (a write is kept, the view follows)
//   +0x350  base frame (3x4, yaw only): copied every frame from the player's heading (a write here is undone)
// The view = base frame x yaw offset x pitch (ELEX.exe+0x312DE9..0x31302F), eased by the game's own camera smoothing.
// The heading: the camera target (+0x78, a static gCCameraTargetMovement) names the player entity (eCEntityProxy at +8:
// handle at +0x10, entity at [handle]); the entity's property set 0x66 (gCCharacterMovement2_PS) points at +0x48 to an
// object whose +0xC is a horizontal unit direction: the base frame's forward row (ELEX.exe+0x309F50 -> 0x411070 ->
// 0x5F0E10). Heading = atan2(x, z), growing to the right, the same sense as GyroLib's yaw.
//
// The yaw. The game's stick never writes the heading: each frame a movement job on a worker thread (ELEX.exe+0x413D80)
// stores a turn (radians, + = right) in a turn struct at +0xF8 of the heading's object, and the main thread applies it
// (0x414B70(movement PS, turn struct, dt): the heading rotated in place). A heading written from outside races that job
// and gets turned back. So 0x414B70 is hooked (found by its first bytes) and the gyro's yaw, queued since its last call,
// is added to the turn (+0x20) right before it is applied, for the player only. Two game quirks are handled there:
//   - The spurious turn: with no turn input the job's wanted heading is the heading itself, bit for bit, yet its angle
//     to it is acosf of a dot that can round just under 1 (0.000345 rad), signed by a cross product of exactly 0, taken
//     as right. In vanilla the heading rests where the dot rounds to 1; each gyro turn moves it to a new value, a third
//     of which give that turn next frame: a creep right of 0.4-1 deg/s while the gyro was on. A turn equal to the
//     heading's angle to itself, computed with the game's own function (0x72DE0, found by its call in the job), is that
//     artifact (real input gives other values) and is dropped. Measured with synthetic tremor: 0.9 deg/s -> 0.02.
//   - The clamp: while aiming (and in some movement turns) the struct's factor (+0x24) is >= 0 and 0x414B70 turns the
//     heading at most by the body's remaining turn (+0x1C, computed before the gyro's yaw): the gyro's yaw is added there
//     too, as if the body's target had moved with it, so aiming follows the gyro 1:1.
// Where 0x414B70 doesn't run for the player (no call for 250 ms), the heading is written directly instead.
// Dead ends (MODLOG): writing the heading directly (lag and the race's creep); adding the yaw to 0x413000's turn input
// argument (0x413720 runs five times a frame and the first call, not the job's, took it).
//
// The FOV: the render camera, a static eCCameraBase (+0xCC vertical FOV, radians: 60 degrees in third person, 45 aiming
// a bow; +0x64 cot(FOV/2), checked to match).
#include "genome.hpp"

#include "engine.hpp"
#include "fov.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "rtti.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace gyro4all::genome {

namespace {

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

constexpr double kPi = 3.14159265358979323846;

// gCCameraAdmin (the static instance in the exe's .data).
constexpr uintptr_t kAdminOwner = 0x30;        // its CProcessor's owner: the admin itself (checked when found)
constexpr uintptr_t kAdminTarget = 0x78;       // gCCameraTargetMovement* (the player), or another camera target
constexpr uintptr_t kAdminCameraName = 0x1C0;  // char*: the active camera's name
constexpr uintptr_t kAdminYaw = 0x39C;         // float degrees: yaw offset (kept at 0 by the game; not written)
constexpr uintptr_t kAdminPitch = 0x3A0;       // float degrees, + looks down
constexpr int kAdminUpdateSlot = 37;           // the per-frame camera update: void(gCCameraAdmin*)
// The player entity, from the camera target.
constexpr uintptr_t kTargetHandle = 0x10;      // gCCameraTargetMovement: eCEntityProxy at +8, its handle at +0x10
constexpr uintptr_t kEntityPsIndex = 0x30;     // eCEntity: byte table, property set type -> slot ([+type+1], 0xFF none)
constexpr uintptr_t kEntityPsArray = 0x38;     // eCEntity: property set pointers by slot
constexpr int kMovementPs = 0x66;              // gCCharacterMovement2_PS
constexpr uintptr_t kMovementLook = 0x48;      // gCCharacterMovement2_PS: the object holding the heading direction
constexpr uintptr_t kLookDirection = 0xC;      // float[3], horizontal unit vector

// The gameplay cameras the gyro turns, by name prefix, and their views. Others seen (no gyro): "cam_ui_world" (inventory
// and other menus), "cam_dialog_npc_1_dist8" (dialogs).
struct GameplayCamera {
    const char* prefix;
    uint32_t view;
};
constexpr GameplayCamera kGameplayCameras[] = {{"cam_normal", 1}, {"cam_ranged", 2}};
// The gyro keeps the pitch within this (degrees). The game's own limits are tighter in some cameras.
constexpr double kMaxPitch = 80;
// The camera update counts as stopped (menus, pause, loading) after this long without a call.
constexpr ULONGLONG kStoppedMs = 250;

uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

// --- the camera update hook ------------------------------------------------------------------------------------------

using UpdateFn = void (*)(void*);
UpdateFn g_update_orig = nullptr;
std::atomic<uint32_t> g_updates{0};

void hooked_update(void* admin) {
    g_updates.fetch_add(1, std::memory_order_relaxed);
    g_update_orig(admin);
}

// --- the player's turn, applied (main thread, like the gyro frame) --------------------------------------------------

// Its start: mov r11, rsp; mov [r11+0x20], rbx; push rbp; push rdi; push r14; lea rbp, [r11-0x5F]; sub rsp, 0xE0;
// movaps [r11-0x38], xmm7; mov rax, [security cookie]; xor rax, rsp; mov [rbp+0xF], rax; mov r14, [rcx+0x48];
// movaps xmm7, xmm2; mov rbx, rdx; mov rdi, rcx.
constexpr const char* kApplyTurnPattern =
    "4C 8B DC 49 89 5B 20 55 57 41 56 49 8D 6B A1 48 81 EC E0 00 00 00 41 0F 29 7B C8 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
    "48 89 45 0F 4C 8B 71 48 0F 28 FA 48 8B DA 48 8B F9";
// The turn struct (+0xF8 of the heading's object).
constexpr uintptr_t kTurnRemaining = 0x1C;  // float radians: the body's remaining turn toward the heading
constexpr uintptr_t kTurn = 0x20;           // float radians, + = right: this frame's turn
constexpr uintptr_t kTurnFactor = 0x24;     // float: -1 = the turn as is; >= 0 = at most the remaining turn (aiming)
constexpr ULONGLONG kTurnInputMs = 250;     // the gyro's yaw goes with the game's turn while it was applied this recently

// The game's angle between two directions (ELEX.exe+0x72DE0: both normalized, dot clamped to [-1, 1], acosf), found by
// its call in the movement job: lea rdx, [rsp+0x50]; lea rcx, [rbp-0x70]; movss [rbp-0x70], xmm7; movss [rbp-0x6C], xmm0;
// movss [rbp-0x68], xmm6; call <angle>.
constexpr const char* kAngleCallPattern = "48 8D 54 24 50 48 8D 4D 90 F3 0F 11 7D 90 F3 0F 11 45 94 F3 0F 11 75 98 E8 ?? ?? ?? ??";

using ApplyTurnFn = void* (*)(void* movement, void* turn_struct, float dt);
using AngleFn = float (*)(const float* a, const float* b);
ApplyTurnFn g_apply_orig = nullptr;
AngleFn g_angle = nullptr;
std::atomic<uintptr_t> g_player_movement{0};  // the player's gCCharacterMovement2_PS
std::atomic<int64_t> g_pending_nano{0};       // the gyro's yaw waiting for the next turn, nano-degrees
std::atomic<ULONGLONG> g_turn_seen{0};         // GetTickCount64 of the last turn applied for the player
std::atomic<uint32_t> g_spurious{0}, g_clamped{0};  // counts for the status line: spurious turns dropped, clamp frames

// The no-input turn's artifact (see the top of the file): a positive turn equal to the heading's angle to itself.
bool spurious_turn(uintptr_t ps, float turn) {
    if (!g_angle || !(turn > 0)) return false;
    float d[3];
    const uintptr_t look = mem::rdp(ps + kMovementLook);
    if (!look || !mem::rd(look + kLookDirection, d)) return false;
    return g_angle(d, d) == turn;
}

void* hooked_apply_turn(void* movement, void* turn_struct, float dt) {
    const auto ps = reinterpret_cast<uintptr_t>(movement);
    const auto ts = reinterpret_cast<uintptr_t>(turn_struct);
    float turn = 0;
    if (ps && ts && ps == g_player_movement.load(std::memory_order_relaxed) && mem::rd(ts + kTurn, turn) &&
        std::isfinite(turn)) {
        g_turn_seen.store(GetTickCount64(), std::memory_order_relaxed);
        if (spurious_turn(ps, turn)) {
            g_spurious.fetch_add(1, std::memory_order_relaxed);
            turn = 0;
            mem::guarded_store(ts + kTurn, &turn, sizeof turn);
        }
        if (const int64_t take = g_pending_nano.exchange(0, std::memory_order_acq_rel)) {
            const double add = double(take) * 1e-9 * kPi / 180;
            const float out = float(double(turn) + add);
            mem::guarded_store(ts + kTurn, &out, sizeof out);
            float factor = -1, remaining = 0;
            if (mem::rd(ts + kTurnFactor, factor) && factor >= 0 && mem::rd(ts + kTurnRemaining, remaining) &&
                std::isfinite(remaining)) {
                const float rem_out = float(double(remaining) + add);
                mem::guarded_store(ts + kTurnRemaining, &rem_out, sizeof rem_out);
                g_clamped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    return g_apply_orig(movement, turn_struct, dt);
}

// --- reading the scene -----------------------------------------------------------------------------------------------

std::string read_name(uintptr_t admin) {
    uintptr_t s = 0;
    char buf[48] = {};
    if (!mem::rd(admin + kAdminCameraName, s) || !s || !mem::plausible(s, 1) || !mem::guarded_copy(buf, s, sizeof buf - 1))
        return {};
    return std::string(buf, strnlen(buf, sizeof buf - 1));
}

bool read_dir(uintptr_t at, double v[3]) {
    float f[3];
    if (!mem::rd(at, f)) return false;
    for (int i = 0; i < 3; ++i) v[i] = f[i];
    const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return std::isfinite(len) && std::fabs(len - 1) < 0.01 && std::fabs(v[1]) < 0.01;
}

double heading_of(const double v[3]) { return std::atan2(v[0], v[2]) * 180 / kPi; }

class GenomeEngine final : public Engine {
public:
    const char* name() const override { return "Genome"; }

    // <root>\system\<game>.exe with <root>\data\ini\ConfigDefault.xml and <root>\data\packed.
    bool detect(const std::wstring& exe_path) const override {
        const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
        const size_t up = dir.find_last_of(L"\\/");
        if (up == std::wstring::npos || _wcsicmp(dir.c_str() + up + 1, L"system") != 0) return false;
        const std::wstring root = dir.substr(0, up);
        return exists(root + L"\\data\\ini\\ConfigDefault.xml") && exists(root + L"\\data\\packed");
    }

    void configure() override {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe_base());
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(exe_base() + dos->e_lfanew);
        log("genome: exe time stamp %lu, image size 0x%lx", nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
        // The vtables exist in the image now; the static camera admin is built later by the exe's CRT start-up, so it
        // is looked for on the game thread.
        const rtti::Image im = rtti::exe_image();
        admin_vtable_ = rtti::primary_vtable(im, ".?AVgCCameraAdmin@@");
        target_vtable_ = rtti::primary_vtable(im, ".?AVgCCameraTargetMovement@@");
        movement_vtable_ = rtti::primary_vtable(im, ".?AVgCCharacterMovement2_PS@@");
        render_vtable_ = rtti::primary_vtable(im, ".?AVeCCameraBase@@");
        log("genome: vtables gCCameraAdmin %s, gCCameraTargetMovement %s, gCCharacterMovement2_PS %s, eCCameraBase %s",
            admin_vtable_ ? mem::module_offset(admin_vtable_).c_str() : "not found",
            target_vtable_ ? mem::module_offset(target_vtable_).c_str() : "not found",
            movement_vtable_ ? mem::module_offset(movement_vtable_).c_str() : "not found",
            render_vtable_ ? mem::module_offset(render_vtable_).c_str() : "not found");
        hook_turn();
        load_zoom_reference();
    }

    void settings_changed() override { load_zoom_reference(); }

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0, true}, {2, "Ranged aim", 1, true}}; }

    EngineFrame read_frame() override {
        EngineFrame f;
        if (!admin_ && !find_admin()) {
            f.target = "camera admin not found yet";
            return f;
        }
        const ULONGLONG now = GetTickCount64();
        const uint32_t updates = g_updates.load(std::memory_order_relaxed);
        if (updates != last_updates_) {
            last_updates_ = updates;
            last_update_at_ = now;
        }
        const bool updating = !g_update_orig || (last_update_at_ && now - last_update_at_ <= kStoppedMs);
        if (updating != updating_ && update_logs_ < 100) {
            ++update_logs_;
            log("genome: camera update %s", updating ? "running" : "stopped (menu, pause or loading)");
        }
        updating_ = updating;

        const std::string cam = read_name(admin_);
        if (cam != camera_name_) {
            if (name_logs_ < 200) {
                ++name_logs_;
                log("genome: camera \"%s\"%s", cam.c_str(), name_logs_ == 200 ? "; further changes not logged" : "");
            }
            camera_name_ = cam;
        }
        uint32_t view = 0;
        for (const GameplayCamera& c : kGameplayCameras)
            if (cam.starts_with(c.prefix)) view = c.view;
        gameplay_camera_ = view != 0;

        resolve_heading();
        g_player_movement.store(movement_, std::memory_order_relaxed);
        f.in_game = heading_ != 0;
        f.paused = !updating_;
        const bool live = updating_ && gameplay_camera_ && heading_;
        f.view_id = live ? view : 0;
        f.target = cam.empty() ? "no camera" : cam;
        if (live) {
            f.zoom_fov = zoom_fov(view);
            track_pitch();
        } else {
            g_pending_nano.store(0, std::memory_order_relaxed);  // no turn queued across menus or cutscenes
        }
        account_heading(live);
        return f;
    }

    // Degrees from GyroLib (pitch up, yaw right). Yaw: queued for the game's own turn (+ = right), or, where that isn't
    // applied for the player, the heading direction turned about the vertical. Pitch: the camera's pitch (+ looks down).
    bool apply(double pitch, double yaw) override {
        if (!heading_ || !admin_) return false;
        double v[3];
        float p = 0;
        if (!read_dir(heading_, v) || !mem::rd(admin_ + kAdminPitch, p) || !std::isfinite(p)) return false;
        const bool via_turn =
            g_apply_orig && GetTickCount64() - g_turn_seen.load(std::memory_order_relaxed) <= kTurnInputMs;
        if (via_turn != via_turn_ && route_logs_ < 50) {
            ++route_logs_;
            log("genome: gyro yaw %s", via_turn ? "added to the game's own turn" : "written to the heading (no turn applied for the player)");
        }
        via_turn_ = via_turn;
        if (via_turn) {
            pending_rest_ += yaw * 1e9;
            const int64_t whole = int64_t(pending_rest_);
            pending_rest_ -= double(whole);
            if (whole) g_pending_nano.fetch_add(whole, std::memory_order_acq_rel);
        } else {
            g_pending_nano.store(0, std::memory_order_relaxed);
            const double c = std::cos(yaw * kPi / 180), s = std::sin(yaw * kPi / 180);
            const double x = c * v[0] + s * v[2], z = -s * v[0] + c * v[2];
            const double len = std::sqrt(x * x + z * z);
            const float out[3] = {float(x / len), 0.0f, float(z / len)};
            if (!mem::guarded_store(heading_, out, sizeof out)) return false;
        }
        const float out_pitch = float(std::clamp(double(p) - pitch, -kMaxPitch, kMaxPitch));
        if (!mem::guarded_store(admin_ + kAdminPitch, &out_pitch, sizeof out_pitch)) return false;
        sum_gyro_ += yaw;
        return true;
    }

    Recenter recenter_support(std::string& detail) override {
        float p = 0;
        if (!heading_ || !admin_ || !mem::rd(admin_ + kAdminPitch, p)) {
            detail = "camera not readable";
            return Recenter::NotYet;
        }
        detail = std::format("{} pitch {:.1f}", camera_name_, -p);
        return Recenter::Yes;
    }

    bool recenter(double fraction) override {
        float p = 0;
        if (!admin_ || !mem::rd(admin_ + kAdminPitch, p)) return false;
        const float out = float(p * (1.0 - fraction));
        return mem::guarded_store(admin_ + kAdminPitch, &out, sizeof out);
    }

    // For the core's stick fallback only. Not while the yaw goes with the game's own turn: the game applies it a frame
    // later, so the next frame's heading doesn't show it yet and the fallback took every turn for an undone write (it
    // then turned the camera through the right stick: a jumpy camera, MODLOG 2026-11-24). Nothing there can be undone;
    // with direct heading writes, the fallback watches as usual.
    bool view_angles(double& pitch, double& yaw, uintptr_t& camera) override {
        if (via_turn_) return false;
        double v[3];
        float p = 0;
        if (!heading_ || !admin_ || !read_dir(heading_, v) || !mem::rd(admin_ + kAdminPitch, p)) return false;
        pitch = -p;
        yaw = heading_of(v);
        camera = heading_;
        return true;
    }

    // Menus and pauses: the camera update stops (pause menu, loading) or another camera takes over (cam_ui_world for the
    // inventory, dialog cameras).
    bool sees_menus() const override { return true; }

    std::string status_scene() const override {
        if (!admin_) return "genome: camera admin not found yet";
        float p = 0, y = 0;
        mem::rd(admin_ + kAdminPitch, p);
        mem::rd(admin_ + kAdminYaw, y);
        double v[3] = {};
        const bool dir = heading_ && read_dir(heading_, v);
        return std::format("genome: camera \"{}\"{}{} heading {} pitch {:.1f} yaw offset {:.1f} (pitch seen {:.1f}..{:.1f})",
                           camera_name_, updating_ ? "" : " (update stopped)", gameplay_camera_ ? "" : " (not gameplay)",
                           dir ? std::format("{:.1f}", heading_of(v)) : std::string("?"), -p, y, -max_pitch_seen_,
                           -min_pitch_seen_);
    }

    // Since the last status line: how far the heading turned in gameplay against the gyro's yaw (the rest is the
    // player's stick or mouse, or drift: about 0 with them idle), the spurious turns dropped and the clamp frames.
    void status_details() override {
        log("status: genome: heading turned %+.1f, gyro %+.1f (%s), rest %+.1f deg; spurious turns dropped %u, clamped "
            "turns opened %u",
            sum_turned_, sum_gyro_, via_turn_ ? "with the game's turn" : "written", sum_turned_ - sum_gyro_,
            g_spurious.exchange(0, std::memory_order_relaxed), g_clamped.exchange(0, std::memory_order_relaxed));
        sum_turned_ = sum_gyro_ = 0;
    }

private:
    uintptr_t admin_vtable_ = 0, target_vtable_ = 0, movement_vtable_ = 0, render_vtable_ = 0;
    uintptr_t admin_ = 0;
    uintptr_t heading_ = 0;   // the heading direction (float[3]) in the player's movement property set's object
    uintptr_t movement_ = 0;  // the player's gCCharacterMovement2_PS
    bool via_turn_ = false;   // the gyro's yaw goes with the game's own turn (else written to the heading)
    int route_logs_ = 0;
    double pending_rest_ = 0;  // nano-degrees not queued yet (under 1)
    ULONGLONG next_search_ = 0;
    uint32_t last_updates_ = 0;
    ULONGLONG last_update_at_ = 0;
    bool updating_ = true;
    bool gameplay_camera_ = false;
    std::string camera_name_;
    int name_logs_ = 0, update_logs_ = 0, heading_logs_ = 0;
    float min_pitch_seen_ = 0, max_pitch_seen_ = 0;
    double last_heading_ = 0;
    uintptr_t last_heading_at_ = 0;  // the heading address last_heading_ belongs to (0: none)
    double sum_turned_ = 0, sum_gyro_ = 0;

    // The static gCCameraAdmin: the qword in the exe's writable data equal to its vtable, whose processor names it as
    // its owner. Looked for once a second until found; then its camera update is hooked (vtable slot).
    bool find_admin() {
        const ULONGLONG now = GetTickCount64();
        if (!admin_vtable_ || now < next_search_) return false;
        next_search_ = now + 1000;
        for (const mem::Range& r : mem::writable_sections()) {
            for (uintptr_t p = r.begin; p + kAdminPitch + 4 <= r.end; p += 8) {
                uintptr_t v = 0;
                if (!mem::rd(p, v) || v != admin_vtable_) continue;
                uintptr_t owner = 0;
                if (!mem::rd(p + kAdminOwner, owner) || owner != p) continue;
                admin_ = p;
                break;
            }
            if (admin_) break;
        }
        if (!admin_) return false;
        void* original = rtti::patch_slot(admin_vtable_, kAdminUpdateSlot, reinterpret_cast<void*>(&hooked_update));
        g_update_orig = reinterpret_cast<UpdateFn>(original);
        log("genome: camera admin %s; camera update (slot %d) %s", mem::module_offset(admin_).c_str(), kAdminUpdateSlot,
            original ? ("hooked, was " + mem::module_offset(reinterpret_cast<uintptr_t>(original))).c_str()
                     : "NOT hooked (no pause detection)");
        return true;
    }

    // The function that applies the movement's turn (main thread), by its first bytes: hooked to add the gyro's yaw;
    // and the game's angle function, by its call in the movement job, for the spurious-turn check.
    void hook_turn() {
        std::vector<uint8_t> bytes, mask;
        mem::parse_pattern(kApplyTurnPattern, bytes, mask);
        const auto hits = mem::find_pattern(mem::code_sections(exe_base()), bytes.data(), mask.data(), bytes.size());
        if (hits.size() != 1) {
            log("genome: turn function not found (%zu matches): gyro yaw written to the heading", hits.size());
            return;
        }
        if (!hooks::hook_function(reinterpret_cast<void*>(hits[0]), reinterpret_cast<void*>(&hooked_apply_turn),
                                  reinterpret_cast<void**>(&g_apply_orig))) {
            g_apply_orig = nullptr;
            log("genome: turn function %s: hook failed; gyro yaw written to the heading", mem::module_offset(hits[0]).c_str());
            return;
        }
        log("genome: turn function %s hooked: gyro yaw added to the game's own turn", mem::module_offset(hits[0]).c_str());
        std::vector<uint8_t> angle_bytes, angle_mask;  // parse_pattern appends: fresh vectors
        mem::parse_pattern(kAngleCallPattern, angle_bytes, angle_mask);
        const auto calls = mem::find_pattern(mem::code_sections(exe_base()), angle_bytes.data(), angle_mask.data(),
                                             angle_bytes.size());
        int32_t rel = 0;
        static const uint8_t kAngleStart[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40};  // push rbx; sub rsp, 0x40
        uint8_t start[sizeof kAngleStart] = {};
        const uintptr_t fn = calls.size() == 1 && mem::rd(calls[0] + 25, rel) ? calls[0] + 29 + rel : 0;
        if (fn && mem::guarded_copy(start, fn, sizeof start) && std::memcmp(start, kAngleStart, sizeof start) == 0) {
            g_angle = reinterpret_cast<AngleFn>(fn);
            log("genome: angle function %s: spurious turns (heading to itself) dropped", mem::module_offset(fn).c_str());
        } else {
            log("genome: angle function not found (%zu calls): spurious turns kept (the camera may creep right)", calls.size());
        }
    }

    // The player's heading direction, from the camera target (every frame: the target and the entity can change).
    void resolve_heading() {
        uintptr_t found = 0, movement = 0;
        const char* why = nullptr;
        uintptr_t target = 0, vt = 0;
        if (!mem::rd(admin_ + kAdminTarget, target) || !mem::plausible(target) || !mem::rd(target, vt) ||
            vt != target_vtable_) {
            why = "no player camera target";
        } else {
            const uintptr_t handle = mem::rdp(target + kTargetHandle);
            const uintptr_t entity = handle ? mem::rdp(handle) : 0;
            const uintptr_t table = entity ? mem::rdp(entity + kEntityPsIndex, 1) : 0;
            uint8_t slot = 0xFF;
            if (!table || !mem::rd(table + kMovementPs + 1, slot) || slot == 0xFF) {
                why = "no player entity or movement";
            } else {
                const uintptr_t ps = mem::rdp(mem::rdp(entity + kEntityPsArray) + uintptr_t(slot) * 8);
                uintptr_t psvt = 0;
                const uintptr_t look = ps && mem::rd(ps, psvt) && psvt == movement_vtable_ ? mem::rdp(ps + kMovementLook) : 0;
                double v[3];
                if (!look) why = "no movement property set";
                else if (!read_dir(look + kLookDirection, v)) why = "heading not a horizontal unit vector";
                else found = look + kLookDirection, movement = ps;
            }
        }
        if (found != heading_ && heading_logs_ < 50) {
            ++heading_logs_;
            log("genome: heading direction %s", found ? mem::module_offset(found).c_str() : why);
        }
        heading_ = found;
        movement_ = movement;
    }

    // The heading's turn in gameplay, for the status line.
    void account_heading(bool live) {
        double v[3];
        if (!live || !read_dir(heading_, v)) {
            last_heading_at_ = 0;
            return;
        }
        const double h = heading_of(v);
        if (last_heading_at_ == heading_) sum_turned_ += std::remainder(h - last_heading_, 360.0);
        last_heading_ = h;
        last_heading_at_ = heading_;
    }

    void track_pitch() {
        float p = 0;
        if (!mem::rd(admin_ + kAdminPitch, p) || !std::isfinite(p)) return;
        min_pitch_seen_ = std::min(min_pitch_seen_, p);
        max_pitch_seen_ = std::max(max_pitch_seen_, p);
    }

    // --- the FOV, for zoom compensation ------------------------------------------------------------------------------
    // The render camera: the first static eCCameraBase in the exe's .data (three copies in a row, all alike); +0xCC its
    // vertical FOV in radians, +0x64 its projection's cot(FOV/2), checked to match. The unzoomed reference:
    // ZoomReferenceFOV= (horizontal degrees), else the FOV the third-person camera (view 1) held steady for kSteadyMs (the
    // game eases the FOV back over about 0.15 s after aiming, already in view 1).
    static constexpr uintptr_t kRenderFov = 0xCC, kRenderCot = 0x64;
    static constexpr ULONGLONG kSteadyMs = 500;
    uintptr_t render_ = 0;
    ULONGLONG next_render_search_ = 0;
    double zoom_reference_ = 0;    // ZoomReferenceFOV= (horizontal degrees), 0: the third-person camera's
    double third_person_fov_ = 0;  // vertical degrees, the last steady one in view 1
    double steady_fov_ = 0;        // view 1: the FOV being timed, and since when it hasn't changed
    ULONGLONG steady_since_ = 0;
    double logged_fov_ = 0;
    int fov_logs_ = 0;
    bool reference_logged_ = false;

    void load_zoom_reference() {
        const double fov = fov::reference_setting();
        if (fov != zoom_reference_ || !reference_logged_)
            log("genome: zoom reference FOV %s", fov > 0 ? (std::to_string(int(fov)) + " degrees horizontal").c_str()
                                                         : "the third-person camera's");
        reference_logged_ = true;
        zoom_reference_ = fov;
    }

    // The render camera's vertical FOV in degrees, 0 if not readable.
    double render_fov() {
        if (!render_ && render_vtable_ && GetTickCount64() >= next_render_search_) {
            next_render_search_ = GetTickCount64() + 1000;
            for (const mem::Range& r : mem::writable_sections()) {
                for (uintptr_t p = r.begin; p + 0x178 <= r.end && !render_; p += 8) {
                    uintptr_t v = 0;
                    if (mem::rd(p, v) && v == render_vtable_) render_ = p;
                }
                if (render_) break;
            }
            if (render_) log("genome: render camera %s (the FOV for zoom compensation)", mem::module_offset(render_).c_str());
        }
        float rad = 0, cot = 0;
        if (!render_ || !mem::rd(render_ + kRenderFov, rad) || !mem::rd(render_ + kRenderCot, cot) || !(rad > 0.05f) ||
            !(rad < 3.0f) || std::fabs(double(cot) * std::tan(double(rad) / 2) - 1) > 0.01)
            return 0;
        return double(rad) * 180 / kPi;
    }

    std::optional<ZoomFov> zoom_fov(uint32_t view) {
        const double current = render_fov();
        if (!fov::is_valid(current)) return std::nullopt;
        if (view == 1) {
            const ULONGLONG now = GetTickCount64();
            if (std::fabs(current - steady_fov_) > 0.05) {
                steady_fov_ = current;
                steady_since_ = now;
            }
            if (!third_person_fov_ || now - steady_since_ >= kSteadyMs) third_person_fov_ = current;
        }
        ZoomFov z;
        z.reference_vertical = zoom_reference_ > 0
                                   ? fov::vertical_from_horizontal(zoom_reference_, fov::game_window_aspect())
                                   : third_person_fov_;
        if (!fov::is_valid(z.reference_vertical)) return std::nullopt;
        z.current_vertical = std::min(current, z.reference_vertical);
        if (std::fabs(current - logged_fov_) > 1 && fov_logs_ < 50) {
            ++fov_logs_;
            logged_fov_ = current;
            log("genome: FOV %.1f degrees vertical (unzoomed %.1f, view %u)", current, z.reference_vertical, view);
        }
        return z;
    }
};

}  // namespace

Engine& engine() {
    static GenomeEngine instance;
    return instance;
}

}  // namespace gyro4all::genome
