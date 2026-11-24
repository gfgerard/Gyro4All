// Genome (Piranha Bytes): ELEX (MODLOG 2026-11-24/25). Native x64 (the 32-bit Risen games can't load this DLL), D3D11,
// MSVC RTTI kept, the engine linked into the exe (<root>\system\ELEX.exe), data in <root>\data\packed. The exe imports
// VERSION.dll (not winmm.dll): install Gyro4All as version.dll. Controllers through XInput 1.3 by ordinal: the sticks
// with XInputGetState, every button with XInputGetKeystroke (the core translates both for a DualSense).
//
// The camera (found with camtrace in the 2018 Steam build, exe time stamp 1521487194; write tests and hardware
// breakpoints): a static gCCameraAdmin in the exe's .data runs the camera once per frame on the main thread (a slot of
// its vtable, called by its CProcessor). +0x1C0 is the active camera's struct, its first field the camera's name (a
// Genome string, char*): "cam_normal" in third person, "cam_ranged"/"cam_ranged_dist" aiming ranged weapons. Its
// camera state (that struct +0x170) holds the yaw offset (+0x6C, degrees; the game keeps 0, not written) and the pitch
// (+0x70, degrees, + looks down: the pitch's source, written directly); +0x20 a yaw-only base frame copied every frame
// from the player's heading. The view = base frame x yaw offset x pitch, eased by the game's own camera smoothing.
// The heading: the camera target (+0x78, a static gCCameraTargetMovement) names the player entity (its eCEntityProxy
// at +8, the proxy's handle at +8, the entity at [handle]); the entity's property set 0x66 (gCCharacterMovement2_PS)
// points at +0x48 to an object whose +0xC is a horizontal unit direction: the base frame's forward row. Heading =
// atan2(x, z), growing to the right, the same sense as GyroLib's yaw.
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
// The layout (MODLOG 2026-11-25): the offsets above are read at start from the game's own instructions, each from code
// found by its class (RTTI vtable) or by a short signature, and checked against a second fact, so another build with
// the same code shape but other offsets still works; what can't be read is left off, with a log line saying what:
//   - camera struct, state, pitch and yaw offsets: the camera frame builder (lea rcx, [rdi+slot]; call <state getter:
//     lea rax, [rcx+state]; ret>; movss xmm7, [rax+pitch] ... movss xmm7, [rbx+yaw]); yaw = pitch - 4;
//   - movement object, heading and the turn struct's fields: the turn function (mov r14, [rcx+look]; movss xmm7,
//     [rbx+turn]; comiss xmm6, [rbx+factor]; movss xmm7, [rbx+remaining]; lea rcx, [r14+heading]); remaining = turn - 4,
//     factor = turn + 4;
//   - the camera target's proxy, the movement property set's type and the entity's property set getter slot: the camera
//     target's vtable slot 2 (lea rdi, [rcx+proxy] ... mov edx, type; call [r8+slot]); the proxy's handle: its
//     GetEntity (vtable slot 1: mov rax, [rcx+handle]; ...; mov rax, [rax]); the entity's property set tables: its
//     property set getter, read from the live entity's own vtable (mov r8, [rcx+index] ... mov rax, [rcx+array]);
//   - the camera update's vtable slot: the camera admin's CProcessor (vtable slot 2: mov rcx, [rcx+owner]; jne <thunk:
//     mov rax, [rcx]; jmp [rax+slot*8]>).
// Still fixed, and checked before use: the admin's processor owner (+0x30, the admin itself), its camera target (+0x78,
// the target's vtable), the render camera's FOV fields (+0xCC, +0x64: one is the cot of half the other).
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

// gCCameraAdmin (the static instance in the exe's .data): fixed offsets, checked before use.
constexpr uintptr_t kAdminOwner = 0x30;   // its CProcessor's owner: the admin itself (checked when found)
constexpr uintptr_t kAdminTarget = 0x78;  // gCCameraTargetMovement* (the player), checked by its vtable

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

// --- reading code ----------------------------------------------------------------------------------------------------

std::vector<uintptr_t> find_code(const char* pattern) {
    std::vector<uint8_t> bytes, mask;
    mem::parse_pattern(pattern, bytes, mask);
    return mem::find_pattern(mem::code_sections(exe_base()), bytes.data(), mask.data(), bytes.size());
}

// The first match of `pattern` in [from, from + len), 0 if none.
uintptr_t find_in(uintptr_t from, size_t len, const char* pattern) {
    if (!from) return 0;
    std::vector<uint8_t> bytes, mask;
    mem::parse_pattern(pattern, bytes, mask);
    const auto hits = mem::find_pattern({mem::Range{from, from + len}}, bytes.data(), mask.data(), bytes.size());
    return hits.empty() ? 0 : hits[0];
}

bool starts_with(uintptr_t at, const char* pattern) { return at && find_in(at, 64, pattern) == at; }

uint8_t rd_u8(uintptr_t at) {
    uint8_t v = 0;
    mem::rd(at, v);
    return v;
}

int32_t rd_i32(uintptr_t at) {
    int32_t v = 0;
    mem::rd(at, v);
    return v;
}

// A function from a vtable (byte offset), 0 unless it points into a loaded module.
uintptr_t vtable_fn(uintptr_t vtable, uintptr_t offset) {
    uintptr_t fn = 0;
    return vtable && mem::rd(vtable + offset, fn) && mem::in_module(fn) ? fn : 0;
}

// The camera frame builder: lea rcx, [rdi+slot]; four movaps saves; call <state getter>; mov rbx, rax;
// movss xmm7, [rax+pitch]; mulss xmm7, [deg to rad].
constexpr const char* kFrameBuilderPattern =
    "48 8D 8F ?? ?? ?? ?? 44 0F 29 A4 24 ?? ?? ?? ?? 44 0F 29 AC 24 ?? ?? ?? ?? 44 0F 29 B4 24 ?? ?? ?? ?? 44 0F 29 BC 24 "
    "?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D8 F3 0F 10 78 ?? F3 0F 59 3D";
// The turn function's start: mov r11, rsp; mov [r11+0x20], rbx; push rbp; push rdi; push r14; lea rbp, [r11-0x5F];
// sub rsp, 0xE0; movaps [r11-0x38], xmm7; mov rax, [security cookie]; xor rax, rsp; mov [rbp+0xF], rax;
// mov r14, [rcx+look]; movaps xmm7, xmm2; mov rbx, rdx; mov rdi, rcx.
constexpr const char* kApplyTurnPattern =
    "4C 8B DC 49 89 5B 20 55 57 41 56 49 8D 6B A1 48 81 EC E0 00 00 00 41 0F 29 7B C8 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
    "48 89 45 0F 4C 8B 71 ?? 0F 28 FA 48 8B DA 48 8B F9";
// The game's angle between two directions (ELEX.exe+0x72DE0: both normalized, dot clamped to [-1, 1], acosf), found by
// its call in the movement job: lea rdx, [rsp+0x50]; lea rcx, [rbp-0x70]; movss [rbp-0x70], xmm7; movss [rbp-0x6C], xmm0;
// movss [rbp-0x68], xmm6; call <angle>.
constexpr const char* kAngleCallPattern = "48 8D 54 24 50 48 8D 4D 90 F3 0F 11 7D 90 F3 0F 11 45 94 F3 0F 11 75 98 E8 ?? ?? ?? ??";
// An entity's property set getter: mov r8, [rcx+index]; test r8, r8; je; movsxd rax, edx; movzx edx, byte [rax+r8+1];
// cmp dl, 0xFF; je; mov rax, [rcx+array].
constexpr const char* kGetPropertySetPattern =
    "4C 8B 41 ?? 4D 85 C0 74 ?? 48 63 C2 42 0F B6 54 00 01 80 FA FF 74 ?? 48 8B 41 ??";

// --- the camera update hook ------------------------------------------------------------------------------------------

using UpdateFn = void (*)(void*);
UpdateFn g_update_orig = nullptr;
std::atomic<uint32_t> g_updates{0};

void hooked_update(void* admin) {
    g_updates.fetch_add(1, std::memory_order_relaxed);
    g_update_orig(admin);
}

// --- the player's turn, applied (main thread, like the gyro frame) --------------------------------------------------

// Read from the turn function at start (see the top of the file); 0 = not known.
uintptr_t g_look = 0;       // gCCharacterMovement2_PS: the object holding the heading
uintptr_t g_heading = 0;    // that object: the heading, float[3]
uintptr_t g_turn = 0, g_remaining = 0, g_factor = 0;  // the turn struct: turn, remaining turn (radians), factor
constexpr ULONGLONG kTurnInputMs = 250;  // the gyro's yaw goes with the game's turn while it was applied this recently

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
    const uintptr_t look = mem::rdp(ps + g_look);
    if (!look || !mem::rd(look + g_heading, d)) return false;
    return g_angle(d, d) == turn;
}

void* hooked_apply_turn(void* movement, void* turn_struct, float dt) {
    const auto ps = reinterpret_cast<uintptr_t>(movement);
    const auto ts = reinterpret_cast<uintptr_t>(turn_struct);
    float turn = 0;
    if (ps && ts && ps == g_player_movement.load(std::memory_order_relaxed) && mem::rd(ts + g_turn, turn) &&
        std::isfinite(turn)) {
        g_turn_seen.store(GetTickCount64(), std::memory_order_relaxed);
        if (spurious_turn(ps, turn)) {
            g_spurious.fetch_add(1, std::memory_order_relaxed);
            turn = 0;
            mem::guarded_store(ts + g_turn, &turn, sizeof turn);
        }
        if (const int64_t take = g_pending_nano.exchange(0, std::memory_order_acq_rel)) {
            const double add = double(take) * 1e-9 * kPi / 180;
            const float out = float(double(turn) + add);
            mem::guarded_store(ts + g_turn, &out, sizeof out);
            float factor = -1, remaining = 0;
            if (mem::rd(ts + g_factor, factor) && factor >= 0 && mem::rd(ts + g_remaining, remaining) &&
                std::isfinite(remaining)) {
                const float rem_out = float(double(remaining) + add);
                mem::guarded_store(ts + g_remaining, &rem_out, sizeof rem_out);
                g_clamped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    return g_apply_orig(movement, turn_struct, dt);
}

// --- reading the scene -----------------------------------------------------------------------------------------------

bool read_dir(uintptr_t at, double v[3]) {
    float f[3];
    if (!mem::rd(at, f)) return false;
    for (int i = 0; i < 3; ++i) v[i] = f[i];
    const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return std::isfinite(len) && std::fabs(len - 1) < 0.01 && std::fabs(v[1]) < 0.01;
}

double heading_of(const double v[3]) { return std::atan2(v[0], v[2]) * 180 / kPi; }

std::string hex(uintptr_t v) { return std::format("0x{:X}", v); }

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
        // The vtables and the code exist in the image now; the static camera admin is built later by the exe's CRT
        // start-up, so it is looked for on the game thread.
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
        read_layout(im);
        hook_turn();
        load_zoom_reference();
    }

    void settings_changed() override { load_zoom_reference(); }

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0, true}, {2, "Ranged aim", 1, true}}; }

    EngineFrame read_frame() override {
        EngineFrame f;
        if (!camera_slot_) {
            f.target = "camera layout not known in this build";
            return f;
        }
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

        const std::string cam = read_name();
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
    // applied for the player, the heading direction turned about the vertical. Pitch: the camera's pitch (+ looks down),
    // when its offset is known and it holds a pitch.
    bool apply(double pitch, double yaw) override {
        if (!heading_ || !admin_) return false;
        double v[3];
        if (!read_dir(heading_, v)) return false;
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
        float p = 0;
        if (read_pitch(p)) {
            const float out_pitch = float(std::clamp(double(p) - pitch, -kMaxPitch, kMaxPitch));
            mem::guarded_store(admin_ + pitch_, &out_pitch, sizeof out_pitch);
        }
        sum_gyro_ += yaw;
        return true;
    }

    Recenter recenter_support(std::string& detail) override {
        float p = 0;
        if (!pitch_) {
            detail = "the camera's pitch isn't known in this build";
            return Recenter::No;
        }
        if (!heading_ || !admin_ || !read_pitch(p)) {
            detail = "camera not readable";
            return Recenter::NotYet;
        }
        detail = std::format("{} pitch {:.1f}", camera_name_, -p);
        return Recenter::Yes;
    }

    bool recenter(double fraction) override {
        float p = 0;
        if (!admin_ || !read_pitch(p)) return false;
        const float out = float(p * (1.0 - fraction));
        return mem::guarded_store(admin_ + pitch_, &out, sizeof out);
    }

    // For the core's stick fallback only. Not while the yaw goes with the game's own turn: the game applies it a frame
    // later, so the next frame's heading doesn't show it yet and the fallback took every turn for an undone write (it
    // then turned the camera through the right stick: a jumpy camera, MODLOG 2026-11-24). Nothing there can be undone;
    // with direct heading writes, the fallback watches as usual.
    bool view_angles(double& pitch, double& yaw, uintptr_t& camera) override {
        if (via_turn_) return false;
        double v[3];
        float p = 0;
        if (!heading_ || !admin_ || !read_dir(heading_, v) || !read_pitch(p)) return false;
        pitch = -p;
        yaw = heading_of(v);
        camera = heading_;
        return true;
    }

    // Menus and pauses: the camera update stops (pause menu, loading) or another camera takes over (cam_ui_world for the
    // inventory, dialog cameras).
    bool sees_menus() const override { return true; }

    std::string status_scene() const override {
        if (!camera_slot_) return "genome: camera layout not known in this build";
        if (!admin_) return "genome: camera admin not found yet";
        float p = 0, y = 0;
        const bool pitch = pitch_ && mem::rd(admin_ + pitch_, p);
        const bool yaw = yaw_ && mem::rd(admin_ + yaw_, y);
        double v[3] = {};
        const bool dir = heading_ && read_dir(heading_, v);
        return std::format("genome: camera \"{}\"{}{} heading {} pitch {} yaw offset {} (pitch seen {:.1f}..{:.1f})",
                           camera_name_, updating_ ? "" : " (update stopped)", gameplay_camera_ ? "" : " (not gameplay)",
                           dir ? std::format("{:.1f}", heading_of(v)) : std::string("?"),
                           pitch ? std::format("{:.1f}", -p) : std::string("?"),
                           yaw ? std::format("{:.1f}", y) : std::string("?"), -max_pitch_seen_, -min_pitch_seen_);
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
    // The layout read from the code (see the top of the file); 0 (or -1) = not known.
    uintptr_t camera_slot_ = 0;   // gCCameraAdmin: the active camera's struct (its name first)
    uintptr_t pitch_ = 0, yaw_ = 0;  // gCCameraAdmin: the camera state's pitch and yaw offset
    uintptr_t target_proxy_ = 0, proxy_handle_ = 0;  // camera target -> eCEntityProxy -> handle
    int movement_type_ = -1;      // gCCharacterMovement2_PS's property set type
    uintptr_t get_ps_offset_ = 0;  // the entity's property set getter in its vtable (byte offset)
    // The live entity's property set tables, read from its own getter (cached by its vtable).
    uintptr_t entity_vtable_ = 0, ps_index_ = 0, ps_array_ = 0;
    int ps_logs_ = 0;

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
    int name_logs_ = 0, update_logs_ = 0, heading_logs_ = 0, pitch_logs_ = 0;
    float min_pitch_seen_ = 0, max_pitch_seen_ = 0;
    double last_heading_ = 0;
    uintptr_t last_heading_at_ = 0;  // the heading address last_heading_ belongs to (0: none)
    double sum_turned_ = 0, sum_gyro_ = 0;

    // --- the layout ----------------------------------------------------------------------------------------------------

    void read_layout(const rtti::Image& im) {
        std::string missing;
        const auto note = [&](const char* what) { missing += missing.empty() ? what : std::string("; ") + what; };

        // The camera struct and its state's pitch and yaw offset, from the camera frame builder.
        const auto frames = find_code(kFrameBuilderPattern);
        if (frames.size() == 1) {
            const uintptr_t at = frames[0];
            const int32_t slot = rd_i32(at + 3);
            const uintptr_t getter = at + 0x30 + rd_i32(at + 0x2C);
            const int32_t state = starts_with(getter, "48 8D 81 ?? ?? ?? ?? C3") ? rd_i32(getter + 3) : -1;
            const uint8_t pitch = rd_u8(at + 0x37);
            const uintptr_t yaw_at = find_in(at + 0x40, 0x80, "E8 ?? ?? ?? ?? F3 0F 10 7B ??");
            const uint8_t yaw = yaw_at ? rd_u8(yaw_at + 9) : 0;
            if (slot > 0 && slot < 0x1000 && state > 0 && state < 0x1000 && yaw_at && yaw + 4 == pitch) {
                camera_slot_ = uintptr_t(slot);
                pitch_ = uintptr_t(slot) + state + pitch;
                yaw_ = uintptr_t(slot) + state + yaw;
            } else {
                note("camera state offsets (code shape differs): no gyro");
            }
        } else {
            note(std::format("camera frame builder ({} matches): no gyro", frames.size()).c_str());
        }

        // The camera target's proxy, the movement property set's type and the property set getter's vtable slot, from
        // the camera target's vtable slot 2; the proxy's handle from eCEntityProxy's slot 1 (GetEntity).
        const uintptr_t target_fn = vtable_fn(target_vtable_, 2 * 8);
        if (starts_with(target_fn, "48 89 5C 24 08 57 48 83 EC ?? 48 8D 79 ?? 48 8B DA")) {
            target_proxy_ = rd_u8(target_fn + 0xD);
            if (const uintptr_t call = find_in(target_fn, 0x100, "BA ?? ?? ?? ?? 4C 8B 00 48 8B C8 41 FF 90 ?? ?? ?? ??")) {
                movement_type_ = rd_i32(call + 1);
                get_ps_offset_ = uintptr_t(rd_i32(call + 14));
            }
        }
        const uintptr_t get_entity = vtable_fn(rtti::primary_vtable(im, ".?AVeCEntityProxy@@"), 1 * 8);
        if (starts_with(get_entity, "48 8B 41 ?? 48 85 C0 74 ?? 48 8B 00 C3")) proxy_handle_ = rd_u8(get_entity + 3);
        if (!target_proxy_ || !proxy_handle_ || movement_type_ < 0 || movement_type_ > 0xFF || !get_ps_offset_ ||
            get_ps_offset_ % 8 || get_ps_offset_ > 0x800) {
            note("player entity chain (camera target, proxy or property set getter): no gyro");
            target_proxy_ = proxy_handle_ = get_ps_offset_ = 0;
            movement_type_ = -1;
        }

        // The camera update's vtable slot, from the admin's CProcessor and the thunk it calls.
        const uintptr_t process = vtable_fn(rtti::primary_vtable(im, ".?AVCProcessor@gCCameraAdmin@@"), 2 * 8);
        if (starts_with(process, "48 8B 49 ?? 48 85 C9 0F 85 ?? ?? ?? ??")) {
            const uintptr_t thunk = process + 13 + rd_i32(process + 9);
            if (starts_with(thunk, "48 8B 01 48 FF A0 ?? ?? ?? ??") && rd_i32(thunk + 6) % 8 == 0)
                update_slot_ = rd_i32(thunk + 6) / 8;
        }
        if (update_slot_ < 0) note("camera update slot: no pause detection");

        log("genome: layout from code: camera struct +%s (pitch +%s, yaw offset +%s), camera target proxy +%s handle +%s, "
            "movement property set %s via vtable +%s, camera update slot %d%s%s",
            hex(camera_slot_).c_str(), hex(pitch_).c_str(), hex(yaw_).c_str(), hex(target_proxy_).c_str(),
            hex(proxy_handle_).c_str(), hex(uintptr_t(movement_type_ < 0 ? 0 : movement_type_)).c_str(),
            hex(get_ps_offset_).c_str(), update_slot_, missing.empty() ? "" : "; not found: ", missing.c_str());
    }
    int update_slot_ = -1;  // the camera update's slot in gCCameraAdmin's vtable

    // The live entity's property set tables, from the property set getter in its own vtable.
    bool ps_tables(uintptr_t entity, uintptr_t& index, uintptr_t& array) {
        uintptr_t vt = 0;
        if (!get_ps_offset_ || !mem::rd(entity, vt) || !vt) return false;
        if (vt != entity_vtable_) {
            entity_vtable_ = vt;
            ps_index_ = ps_array_ = 0;
            const uintptr_t getter = vtable_fn(vt, get_ps_offset_);
            if (starts_with(getter, kGetPropertySetPattern)) {
                ps_index_ = rd_u8(getter + 3);
                ps_array_ = rd_u8(getter + 0x1A);
            }
            if (ps_logs_ < 20) {
                ++ps_logs_;
                log("genome: player entity %s: property set tables %s", rtti::class_name(entity).c_str(),
                    ps_index_ ? std::format("+{} / +{}", hex(ps_index_), hex(ps_array_)).c_str()
                              : "not readable from its getter (no gyro)");
            }
        }
        index = ps_index_;
        array = ps_array_;
        return index && array;
    }

    // --- the scene -----------------------------------------------------------------------------------------------------

    std::string read_name() const {
        uintptr_t s = 0;
        char buf[48] = {};
        if (!mem::rd(admin_ + camera_slot_, s) || !s || !mem::plausible(s, 1) || !mem::guarded_copy(buf, s, sizeof buf - 1))
            return {};
        return std::string(buf, strnlen(buf, sizeof buf - 1));
    }

    // The camera's pitch, if its offset is known and it holds a pitch (else the layout doesn't fit: not written).
    bool read_pitch(float& p) {
        if (!pitch_ || !admin_ || !mem::rd(admin_ + pitch_, p) || !std::isfinite(p)) return false;
        if (std::fabs(p) <= 90) return true;
        if (pitch_logs_ < 5) {
            ++pitch_logs_;
            log("genome: the camera's pitch at +%s reads %.1f: not a pitch, not written", hex(pitch_).c_str(), p);
        }
        return false;
    }

    // The static gCCameraAdmin: the qword in the exe's writable data equal to its vtable, whose processor names it as
    // its owner. Looked for once a second until found; then its camera update is hooked (vtable slot).
    bool find_admin() {
        const ULONGLONG now = GetTickCount64();
        if (!admin_vtable_ || now < next_search_) return false;
        next_search_ = now + 1000;
        for (const mem::Range& r : mem::writable_sections()) {
            for (uintptr_t p = r.begin; p + kAdminTarget + 8 <= r.end; p += 8) {
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
        void* original = update_slot_ >= 0 ? rtti::patch_slot(admin_vtable_, update_slot_, reinterpret_cast<void*>(&hooked_update))
                                           : nullptr;
        g_update_orig = reinterpret_cast<UpdateFn>(original);
        log("genome: camera admin %s; camera update (slot %d) %s", mem::module_offset(admin_).c_str(), update_slot_,
            original ? ("hooked, was " + mem::module_offset(reinterpret_cast<uintptr_t>(original))).c_str()
                     : "NOT hooked (no pause detection)");
        return true;
    }

    // The function that applies the movement's turn (main thread), by its first bytes: the movement object's offset,
    // the heading's and the turn struct's fields read from its code, then hooked to add the gyro's yaw; and the game's
    // angle function, by its call in the movement job, for the spurious-turn check.
    void hook_turn() {
        const auto hits = find_code(kApplyTurnPattern);
        if (hits.size() != 1) {
            log("genome: turn function not found (%zu matches): no gyro (the heading's offsets come from it)", hits.size());
            return;
        }
        const uintptr_t fn = hits[0];
        const uintptr_t look = rd_u8(fn + 0x2C);
        const uintptr_t turn = find_in(fn, 0x300, "F3 0F 10 7B ?? 0F 2E FE 0F 84 ?? ?? ?? ?? 0F 2F 73 ??");
        const uintptr_t remaining = find_in(fn, 0x300, "F3 0F 10 7B ?? 0F 2E FE 74");
        const uintptr_t heading = find_in(fn, 0x300, "49 8D 4E ?? 48 8D 55 ?? 0F 28 CE");
        if (!look || !heading) {
            log("genome: turn function %s: the heading's offsets not readable: no gyro", mem::module_offset(fn).c_str());
            return;
        }
        g_look = look;
        g_heading = rd_u8(heading + 3);
        const uint8_t t = turn ? rd_u8(turn + 4) : 0, f = turn ? rd_u8(turn + 0x11) : 0, r = remaining ? rd_u8(remaining + 4) : 0;
        if (!turn || !remaining || r + 4 != t || f != t + 4) {
            log("genome: turn function %s: the turn struct's fields not readable: gyro yaw written to the heading",
                mem::module_offset(fn).c_str());
            return;
        }
        g_turn = t;
        g_remaining = r;
        g_factor = f;
        if (!hooks::hook_function(reinterpret_cast<void*>(fn), reinterpret_cast<void*>(&hooked_apply_turn),
                                  reinterpret_cast<void**>(&g_apply_orig))) {
            g_apply_orig = nullptr;
            log("genome: turn function %s: hook failed; gyro yaw written to the heading", mem::module_offset(fn).c_str());
            return;
        }
        log("genome: turn function %s hooked: gyro yaw added to the game's own turn (movement +%s, heading +%s, turn +%s, "
            "remaining +%s, factor +%s)",
            mem::module_offset(fn).c_str(), hex(g_look).c_str(), hex(g_heading).c_str(), hex(g_turn).c_str(),
            hex(g_remaining).c_str(), hex(g_factor).c_str());
        const auto calls = find_code(kAngleCallPattern);
        const uintptr_t angle = calls.size() == 1 ? calls[0] + 29 + rd_i32(calls[0] + 25) : 0;
        if (starts_with(angle, "40 53 48 83 EC 40")) {  // push rbx; sub rsp, 0x40
            g_angle = reinterpret_cast<AngleFn>(angle);
            log("genome: angle function %s: spurious turns (heading to itself) dropped", mem::module_offset(angle).c_str());
        } else {
            log("genome: angle function not found (%zu calls): spurious turns kept (the camera may creep right)", calls.size());
        }
    }

    // The player's heading direction, from the camera target (every frame: the target and the entity can change).
    void resolve_heading() {
        uintptr_t found = 0, movement = 0;
        const char* why = nullptr;
        uintptr_t target = 0, vt = 0, index = 0, array = 0;
        if (!g_look || !g_heading || !target_proxy_) {
            why = "the heading's offsets aren't known in this build";
        } else if (!mem::rd(admin_ + kAdminTarget, target) || !mem::plausible(target) || !mem::rd(target, vt) ||
                   vt != target_vtable_) {
            why = "no player camera target";
        } else {
            const uintptr_t handle = mem::rdp(target + target_proxy_ + proxy_handle_);
            const uintptr_t entity = handle ? mem::rdp(handle) : 0;
            const uintptr_t table = entity && ps_tables(entity, index, array) ? mem::rdp(entity + index, 1) : 0;
            uint8_t slot = 0xFF;
            if (!table || !mem::rd(table + uintptr_t(movement_type_) + 1, slot) || slot == 0xFF) {
                why = "no player entity or movement";
            } else {
                const uintptr_t ps = mem::rdp(mem::rdp(entity + array) + uintptr_t(slot) * 8);
                uintptr_t psvt = 0;
                const uintptr_t look = ps && mem::rd(ps, psvt) && psvt == movement_vtable_ ? mem::rdp(ps + g_look) : 0;
                double v[3];
                if (!look) why = "no movement property set";
                else if (!read_dir(look + g_heading, v)) why = "heading not a horizontal unit vector";
                else found = look + g_heading, movement = ps;
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
        if (!read_pitch(p)) return;
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
