// Genome (Piranha Bytes): ELEX, ELEX 2 and Risen 3 (MODLOG 2026-11-24/25), and Risen through its own route
// (genome_risen1.cpp: the older engine generation, nothing below applies to it). Native x64 (Risen's 2023 release, Risen
// 3's 2015 Enhanced Edition; the 32-bit Risen 2 and the original Risen/Risen 3 can't load this DLL), D3D9 (Risen 3),
// D3D11 (ELEX) or D3D12 (ELEX 2), MSVC RTTI kept, the engine linked into the exe (<root>\system\ELEX.exe, ELEX2.exe, Risen3.exe), data in
// <root>\data\packed. The exes import VERSION.dll (Risen 3 also winmm.dll): install Gyro4All as version.dll.
// Controllers through XInput 1.3 by ordinal: the sticks with XInputGetState, every button with XInputGetKeystroke (the
// core translates both for a DualSense). Offsets below: ELEX (2018 Steam exe, time stamp 1521487194) / ELEX 2 (2022 Steam
// exe, time stamp 1652720224).
//
// The camera (found with camtrace in ELEX; write tests and hardware breakpoints): a static gCCameraAdmin in the exe's
// .data runs the camera once per frame (a slot of its vtable, called through its CProcessor). +0x1C0/+0x1E8 is the
// active camera's struct, its first field the camera's name (a Genome string, char*): ELEX "cam_normal" in third person,
// "cam_ranged"/"cam_ranged_dist" aiming ranged weapons; ELEX 2 "cam_game_alt" for both (aiming only moves the camera
// closer, no FOV change: no aim view). Its camera state (that struct +0x170/+0x158) holds the yaw offset (+0x6C/+0xE0,
// degrees; the game keeps 0, not written) and the pitch (+0x70/+0xFC, degrees, + looks down: the pitch's source, written
// directly). The view = base frame (the player's heading) x yaw offset x pitch, eased by the game's own camera smoothing.
// The heading: the camera target (admin +0x78/+0x80, a static gCCameraTargetMovement) names the player entity (its
// eCEntityProxy at +8, the proxy's handle at +8/+0, the entity at [handle]); the entity's property set 0x66
// (gCCharacterMovement2_PS) points at +0x48 to an object whose +0xC/+8 is a horizontal unit direction. Heading =
// atan2(x, z), growing to the right, the same sense as GyroLib's yaw.
//
// The yaw. The game's stick never writes the heading: each frame a movement job on a worker thread (ELEX.exe+0x413D80)
// stores a turn (radians, + = right) in a turn struct, and a later call applies it (ELEX.exe+0x414B70 /
// ELEX2.exe+0x5F5790 (movement PS, turn struct, dt): the heading rotated in place; ELEX: main thread, ELEX 2: job
// threads). A heading written from outside races that job and gets turned back. So that function is hooked (found by its
// first bytes) and the gyro's yaw, queued since its last call, turns the heading right after the game's own turn, in the
// same call, for the player only. Not through the turn struct (MODLOG 2026-11-25): when its factor (+0x24) is >= 0 the
// game cuts a turn past the body's remaining turn (+0x1C) to remaining x factor; ELEX sets it while aiming (the gyro's
// yaw was cut to almost nothing), ELEX 2 always (10): adding the yaw to the remaining turn too made every stick turn
// alternate between full and 10 x the gyro's yaw (a stutter).
// The spurious turn, a game bug handled there: with no turn input the job's wanted heading is the heading itself, bit for
// bit, yet its angle to it is acosf of a dot that can round just under 1 (0.000345 rad), signed by a cross product of
// exactly 0, taken as right. In vanilla the heading rests where the dot rounds to 1; each gyro turn moves it to a new
// value, a third of which give that turn next frame: a creep right of 0.4-1 deg/s while the gyro was on. A turn equal to
// the heading's angle to itself, computed with the game's own function (ELEX.exe+0x72DE0 / ELEX2.exe+0x5E00D0, found by
// its call in the job), is that artifact (real input gives other values) and is dropped. Measured with synthetic
// tremor: ELEX 0.9 deg/s -> 0.02, ELEX 2 0.5 -> 0.07.
// Where the turn function doesn't run for the player (no call for 250 ms), the heading is written directly instead.
// Dead ends (MODLOG): writing the heading directly (lag and the race's creep); adding the yaw to 0x413000's turn input
// argument (0x413720 runs five times a frame and the first call, not the job's, took it); adding it to the turn struct.
//
// Risen 3 (Steam, time stamp 1437990428; the same camera and movement code, built with VS2010): camera struct +0x1C0,
// state +0x130 (pitch +0x70, yaw offset +0x6C: admin +0x360), camera target admin +0x78, the movement property set's type
// 0x69 (vtable +0xD8), the heading in the property set itself (+0x48, no separate object), the turn function
// Risen3.exe+0x3383A0, the angle function +0x1B9D30, and the camera update called directly by the CProcessor
// (Risen3.exe+0x1C6810, hooked inline). Its exe carries Steam's DRM wrapper: the code is read on the first frame.
//
// Menus: the camera update stops (pause, loading) or another camera takes over (ELEX: cam_ui_world for the inventory,
// dialog cameras); and the UI's state stack (gCUIModule -> gCUISystem: "Game", then "PauseMenu", "GameMenu" (ELEX 2's
// inventory, map and skills), "QuickSlotWheel" (ELEX 2's weapon wheel), "GameWorld" (ELEX's inventory) on top) turns the
// gyro off whenever anything is on top of "Game": ELEX 2 keeps its gameplay camera behind its menus.
//
// Aiming (the view "Ranged aim", e.g. for gyro only while aiming): ELEX and Risen 3 aim with a camera of their own
// (cam_ranged*). ELEX 2 aims with its gameplay camera (only its target distance shrinks: 250 -> 100 with a pistol), so its
// aim mode decides (MODLOG 2026-11-25): gCCombatSystem2_PS (the player's property set 0x57) -> aim object (+0xA0): state
// (+4: 0 idle, 1 aiming, 2 shooting) or the cool-down after a shot (+0xB0, seconds), as its IsInAimMode reads them (offsets
// read from that code; the set found by its class). The view follows the mode frame by frame: aiming and shooting with
// RT drops it between shots, as the game's own zoom does (a 0.75 s linger was tried and dropped: the user aims with LB).
//
// The layout (MODLOG 2026-11-25): the offsets above are read at start from the game's own instructions, each from code
// found by its class (RTTI vtable) or by a short signature, so another build with the same code shape but other offsets
// still works; each finder knows the code shapes of both games (kFrameShapes, kTurnShapes, ...). What can't be read is
// left off, with a log line saying what:
//   - camera struct, state, pitch and yaw offsets and the camera target: the camera frame builder (lea rcx,
//     [reg+slot]; call <state getter: lea rax, [rcx+state]; ret>; movss xmm, [rax+pitch] ... movss xmm, [reg+yaw]) and
//     the camera target's store just before it (cmp reg, [admin+target]; je; mov [admin+target], reg);
//   - movement object, heading and the turn: the turn function (mov reg, [rcx+look]; movss xmm, [rbx+turn]; ucomiss;
//     je; comiss xmm, [rbx+factor]; movss xmm, [rbx+remaining]; lea rcx, [reg+heading]); checked: remaining = turn - 4,
//     factor = turn + 4;
//   - the camera target's proxy, the movement property set's type and the entity's property set getter slot: the camera
//     target's vtable slot 2 (lea rdi, [rcx+proxy] ... call <GetEntity>; mov edx, type; call [r8+slot]); the proxy's
//     handle: that GetEntity (mov rax, [rcx+handle]; ...; mov rax, [rax]; ELEX: virtual, eCEntityProxy's slot 1); the
//     entity's property set tables: its property set getter, read from the live entity's own vtable (mov r8,
//     [rcx+index] ... mov rax, [rcx+array]);
//   - the camera update's vtable slot (37/40): the camera admin's CProcessor (vtable slot 2: the process callback, or
//     ELEX 2 a scheduler call with it; the callback: mov rcx, [rcx+owner]; jne <thunk: mov rax, [rcx]; jmp [rax+slot*8]>);
//   - the UI's system and state stack: from the live objects, by their classes.
// Still fixed, and checked before use: the admin's processor owner (+0x30, the admin itself), the render camera's FOV
// fields (+0xCC, +0x64: one is the cot of half the other), a UI state's name (+0x10, char*).
//
// The FOV: the render camera, a static eCCameraBase (+0xCC vertical FOV, radians: ELEX 60 degrees in third person, 45
// aiming a bow; ELEX 2 65, unchanged when aiming; +0x64 cot(FOV/2), checked to match).
#include "genome.hpp"
#include "genome_risen1.hpp"

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

// gCCameraAdmin (the static instance in the exe's .data): its CProcessor's owner (the admin itself), a fixed offset in
// both games, checked when the admin is found.
constexpr uintptr_t kAdminOwner = 0x30;

// The gameplay cameras the gyro turns, by name prefix, and their views: ELEX cam_normal, cam_ranged*; ELEX 2 cam_game_alt
// (also aiming). Others seen (no gyro): "cam_ui_world" (ELEX's inventory and other menus), "cam_dialog_npc_1_dist8"
// (dialogs).
struct GameplayCamera {
    const char* prefix;
    uint32_t view;
};
constexpr GameplayCamera kGameplayCameras[] = {{"cam_normal", 1}, {"cam_game", 1}, {"cam_ranged", 2}};
// The gyro keeps the pitch within this (degrees). The game's own limits are tighter in some cameras.
constexpr double kMaxPitch = 80;
// The camera update counts as stopped (menus, pause, loading) after this long without a call.
constexpr ULONGLONG kStoppedMs = 250;

uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

// The game is Risen (its own route, genome_risen1.cpp).
bool risen1_exe() {
    wchar_t exe[MAX_PATH] = {};
    return GetModuleFileNameW(nullptr, exe, MAX_PATH) && risen1::detect(exe);
}

// The exe carries Steam's DRM wrapper (a ".bind" section; Risen 3): its code is encrypted until the wrapper's entry
// point has run, after this DLL's load, so the code is read on the first frame instead.
bool steam_drm() {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe_base());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(exe_base() + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (std::memcmp(sec[i].Name, ".bind", 6) == 0) return true;
    return false;
}

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

// The target of a call or jump whose rel32 is at `at`.
uintptr_t rel_target(uintptr_t at) { return at ? at + 4 + rd_i32(at) : 0; }

// --- the code shapes ---------------------------------------------------------------------------------------------
// Each finder knows the shapes the compiler gave its code in ELEX (2018 Steam exe) and ELEX 2 (2022 Steam exe); a
// shape's pattern must match exactly once, and its fields are read at fixed places in the match.

// The camera layout from the camera frame builder and the code just before it (MODLOG 2026-11-25).
struct CameraLayout {
    int32_t slot = -1;    // gCCameraAdmin: the active camera's struct (lea rcx, [admin+slot] before the state getter)
    int32_t state = -1;   // that struct: the camera state (the getter: lea rax, [rcx+state]; ret)
    int32_t pitch = -1;   // the state: the pitch (the first angle read, times deg to rad)
    int32_t yaw = -1;     // the state: the yaw offset (the second; status line only)
    int32_t target = -1;  // gCCameraAdmin: the camera target (cmp reg, [admin+target]; je; mov [admin+target], reg)
};

int32_t state_of_getter(uintptr_t getter) {
    return starts_with(getter, "48 8D 81 ?? ?? ?? ?? C3") ? rd_i32(getter + 3) : -1;
}

struct FrameShape {
    const char* game;
    const char* pattern;
    void (*read)(uintptr_t at, CameraLayout& l);
};
constexpr FrameShape kFrameShapes[] = {
    // ELEX: lea rcx, [rdi+slot]; four movaps saves; call <state getter>; mov rbx, rax; movss xmm7, [rax+pitch];
    // mulss xmm7, [deg to rad] ... call; movss xmm7, [rbx+yaw]. Before it: cmp rbx, [rdi+target]; je;
    // mov [rdi+target], rbx; test rbx, rbx.
    {"ELEX",
     "48 8D 8F ?? ?? ?? ?? 44 0F 29 A4 24 ?? ?? ?? ?? 44 0F 29 AC 24 ?? ?? ?? ?? 44 0F 29 B4 24 ?? ?? ?? ?? 44 0F 29 BC 24 "
     "?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D8 F3 0F 10 78 ?? F3 0F 59 3D",
     [](uintptr_t at, CameraLayout& l) {
         l.slot = rd_i32(at + 3);
         l.state = state_of_getter(rel_target(at + 0x2C));
         l.pitch = rd_u8(at + 0x37);
         if (const uintptr_t y = find_in(at + 0x40, 0x80, "E8 ?? ?? ?? ?? F3 0F 10 7B ??")) l.yaw = rd_u8(y + 9);
         const uintptr_t t = find_in(at - 0x100, 0x100, "48 3B 5F ?? 74 ?? 48 89 5F ?? 48 85 DB");
         if (t && rd_u8(t + 3) == rd_u8(t + 9)) l.target = rd_u8(t + 3);
     }},
    // ELEX 2: lea rcx, [rbx+slot]; five movaps saves; call <state getter>; lea rcx, [rbp-x]; mov rdi, rax;
    // movss xmm1, [rax+pitch]; mulss xmm1, [deg to rad]; call; movss xmm6, [rdi+yaw]. Before it: cmp rdi,
    // [rbx+target]; je; mov [rbx+target], rdi (disp32: the target moved to +0x80).
    {"ELEX 2",
     "48 8D 8B ?? ?? ?? ?? 0F 29 BC 24 ?? ?? ?? ?? 44 0F 29 8C 24 ?? ?? ?? ?? 44 0F 29 94 24 ?? ?? ?? ?? 44 0F 29 A4 24 "
     "?? ?? ?? ?? 44 0F 29 AC 24 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 4D ?? 48 8B F8 F3 0F 10 88 ?? ?? ?? ?? F3 0F 59 0D "
     "?? ?? ?? ?? E8 ?? ?? ?? ?? F3 0F 10 B7 ?? ?? ?? ??",
     [](uintptr_t at, CameraLayout& l) {
         l.slot = rd_i32(at + 3);
         l.state = state_of_getter(rel_target(at + 0x34));
         l.pitch = rd_i32(at + 0x43);
         l.yaw = rd_i32(at + 0x58);
         const uintptr_t t = find_in(at - 0x100, 0x100, "48 3B BB ?? ?? ?? ?? 74 ?? 48 89 BB ?? ?? ?? ??");
         if (t && rd_i32(t + 3) == rd_i32(t + 0xC)) l.target = rd_i32(t + 3);
     }},
    // Risen 3: lea rcx, [rbx+slot]; call <state getter>; lea rcx, [rbp-x]; movss xmm1, [rax+pitch]; mulss xmm1,
    // [deg to rad]; call; the same again for the yaw (movss xmm1, [rax+yaw]). Before it: cmp rdi, [rbx+target]; je;
    // mov [rbx+target], rdi; test rdi, rdi.
    {"Risen 3",
     "48 8D 8B ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 4D ?? F3 0F 10 48 ?? F3 0F 59 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 8B ?? ?? "
     "?? ?? E8 ?? ?? ?? ?? 48 8D 4D ?? F3 0F 10 48 ??",
     [](uintptr_t at, CameraLayout& l) {
         if (rd_i32(at + 0x25) != rd_i32(at + 3) || rel_target(at + 0x2A) != rel_target(at + 8)) return;
         l.slot = rd_i32(at + 3);
         l.state = state_of_getter(rel_target(at + 8));
         l.pitch = rd_u8(at + 0x14);
         l.yaw = rd_u8(at + 0x36);
         const uintptr_t t = find_in(at - 0x100, 0x100, "48 3B 7B ?? 74 ?? 48 89 7B ?? 48 85 FF");
         if (t && rd_u8(t + 3) == rd_u8(t + 9)) l.target = rd_u8(t + 3);
     }},
};

// The function that applies the movement's turn: its start (the movement object's offset at look_at: mov reg,
// [rcx+look]; 0: no such object, the heading is in the movement property set itself, Risen 3), and in its code: `turn`
// (movss xmmA, [rbx+turn]; ucomiss xmmA, xmmB; je; comiss xmmB, [rbx+factor]: turn at +4, factor at +0x11),
// `remaining` (movss xmmA, [rbx+remaining]; ... ucomiss; je short: at +4) and `heading` (lea rcx, [look reg+heading]
// for the rotation: at +3).
struct TurnShape {
    const char* game;
    const char* pattern;
    uint8_t look_at;
    const char* turn;
    const char* remaining;
    const char* heading;
};
constexpr TurnShape kTurnShapes[] = {
    // mov r11, rsp; mov [r11+0x20], rbx; push rbp; push rdi; push r14; lea rbp, [r11-0x5F]; sub rsp, 0xE0;
    // movaps [r11-0x38], xmm7; mov rax, [security cookie]; xor rax, rsp; mov [rbp+0xF], rax; mov r14, [rcx+look];
    // movaps xmm7, xmm2; mov rbx, rdx; mov rdi, rcx.
    {"ELEX",
     "4C 8B DC 49 89 5B 20 55 57 41 56 49 8D 6B A1 48 81 EC E0 00 00 00 41 0F 29 7B C8 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
     "48 89 45 0F 4C 8B 71 ?? 0F 28 FA 48 8B DA 48 8B F9",
     0x2C, "F3 0F 10 7B ?? 0F 2E FE 0F 84 ?? ?? ?? ?? 0F 2F 73 ??", "F3 0F 10 7B ?? 0F 2E FE 74",
     "49 8D 4E ?? 48 8D 55 ?? 0F 28 CE"},
    // mov r11, rsp; mov [r11+0x18], rbx; push rbp; push rsi; push rdi; lea rbp, [r11-0x5F]; sub rsp, 0x100;
    // movaps [r11-0x48], xmm8; mov rax, [security cookie]; xor rax, rsp; mov [rbp+7], rax; mov rsi, [rcx+look];
    // movaps xmm8, xmm2; mov rbx, rdx; mov rdi, rcx.
    {"ELEX 2",
     "4C 8B DC 49 89 5B 18 55 56 57 49 8D 6B A1 48 81 EC 00 01 00 00 45 0F 29 43 B8 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
     "48 89 45 07 48 8B 71 ?? 44 0F 28 C2 48 8B DA 48 8B F9",
     0x2B, "F3 0F 10 73 ?? 0F 2E F7 0F 84 ?? ?? ?? ?? 0F 2F 7B ??", "F3 0F 10 73 ?? 0F 2E F7 74",
     "48 8D 4E ?? 0F 28 C8 F3 0F 11 55 ?? 0F 57 0D"},
    // mov rax, rsp; mov [rax+0x18], rbx; push rbp; push rsi; push rdi; lea rbp, [rax-0x5F]; sub rsp, 0xF0; four movaps
    // saves; mov rax, [security cookie]; xor rax, rsp; mov [rbp-1], rax; xor esi, esi; test byte [rcx+0x18], 8;
    // movaps xmm7, xmm2; mov rbx, rdx; mov rdi, rcx. The heading: lea rcx, [rdi+heading] (the property set itself);
    // remaining: movss xmm7, [rbx+remaining]; comiss; jae; mov al, 1; jmp; xor al, al; ucomiss; je.
    {"Risen 3",
     "48 8B C4 48 89 58 18 55 56 57 48 8D 68 A1 48 81 EC ?? ?? ?? ?? 0F 29 70 D8 0F 29 78 C8 44 0F 29 40 B8 44 0F 29 48 "
     "A8 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 FF 33 F6 F6 41 18 08 0F 28 FA 48 8B DA 48 8B F9",
     0, "F3 0F 10 7B ?? 0F 2E FE 0F 84 ?? ?? ?? ?? 0F 2F 73 ??", "F3 0F 10 7B ?? 0F 2F FE 73 04 B0 01 EB 02 32 C0 0F 2E FE 74",
     "48 8D 4F ?? 48 8D 55 ?? F3 0F 11 75"},
};

// The game's angle between two directions (both normalized, dot clamped to [-1, 1], acosf; ELEX.exe+0x72DE0,
// ELEX2.exe+0x5E00D0, Risen3.exe+0x1B9D30), found by its call in the movement job (call's rel32 at call_at). heading_at: where the shape
// names the heading's offset (checked against the turn function's), 0 if it doesn't.
struct AngleShape {
    const char* game;
    const char* pattern;
    uint8_t call_at;
    uint8_t heading_at;
};
constexpr AngleShape kAngleShapes[] = {
    // lea rdx, [rsp+0x50]; lea rcx, [rbp-0x70]; movss [rbp-0x70], xmm7; movss [rbp-0x6C], xmm0; movss [rbp-0x68], xmm6;
    // call <angle>.
    {"ELEX", "48 8D 54 24 50 48 8D 4D 90 F3 0F 11 7D 90 F3 0F 11 45 94 F3 0F 11 75 98 E8 ?? ?? ?? ??", 25, 0},
    // add rax, heading; jmp; lea rax, [zero vector]; movsd xmm0, [rax]; lea rdx, [rsp+a]; mov eax, [rax+8];
    // lea rcx, [rsp+b]; movsd [rsp+b], xmm0; mov [rsp+b+8], eax; call <angle> (the heading, copied, against the wanted one).
    {"ELEX 2",
     "48 83 C0 ?? EB ?? 48 8D 05 ?? ?? ?? ?? F2 0F 10 00 48 8D 54 24 ?? 8B 40 08 48 8D 4C 24 ?? F2 0F 11 44 24 ?? 89 44 "
     "24 ?? E8 ?? ?? ?? ??",
     41, 3},
    // The heading copied from the property set (mov eax, [rbx+heading]; mov [rsp+a], eax; three times), then call
    // <angle> (the heading against the wanted one).
    {"Risen 3", "8B 43 ?? 89 44 24 ?? 8B 43 ?? 89 44 24 ?? 8B 43 ?? 89 44 24 ?? E8 ?? ?? ?? ??", 22, 2},
};

// The camera target's vtable slot 2 (GetMatrix): near its start lea rdi (ELEX) or rsi (Risen 3), [rcx+proxy]; mov rbx,
// rdx; later the movement property set's lookup: call <GetEntity(proxy)>; mov edx, type; (mov r8, [rax]; mov rcx, rax,
// either order); call [r8+slot].
constexpr const char* kTargetProxyPatterns[] = {"48 8D 79 ?? 48 8B DA", "48 8D 71 ?? 48 8B DA"};
constexpr const char* kPropertySetCallPatterns[] = {
    "BA ?? ?? ?? ?? 4C 8B 00 48 8B C8 41 FF 90 ?? ?? ?? ??",  // ELEX
    "BA ?? ?? ?? ?? 48 8B C8 4C 8B 00 41 FF 90 ?? ?? ?? ??",  // ELEX 2
};
// An entity's property set getter: mov r8, [rcx+index]; test r8, r8; je; movsxd rax, edx; movzx edx, byte [rax+r8+1];
// cmp dl, 0xFF; je; mov rax, [rcx+array].
constexpr const char* kGetPropertySetPattern =
    "4C 8B 41 ?? 4D 85 C0 74 ?? 48 63 C2 42 0F B6 54 00 01 80 FA FF 74 ?? 48 8B 41 ??";
// eCEntityProxy::GetEntity: mov rax, [rcx+handle]; test rax, rax; je; mov rax, [rax]; ret (ELEX: virtual, slot 1, reached
// through a thunk mov rax, [rcx]; jmp [rax+8]; ELEX 2: called directly, handle at +0: mov rax, [rcx]; ...).
constexpr const char* kGetEntityPattern = "48 8B 41 ?? 48 85 C0 74 ?? 48 8B 00 C3";
constexpr const char* kGetEntityAt0Pattern = "48 8B 01 48 85 C0 74 ?? 48 8B 00 C3";
constexpr const char* kGetEntityThunkPattern = "48 8B 01 48 FF 60 08";
// The camera admin's CProcessor, vtable slot 2: the process callback itself (ELEX: mov rcx, [rcx+owner]; test rcx, rcx;
// jne <thunk>), or (ELEX 2) a call through the scheduler with it: mov rax, [rdx]; mov r9, rdx; mov r8, rcx;
// lea rdx, [callback]; mov rcx, r9; jmp [rax+8]. The thunk: mov rax, [rcx]; jmp [rax+slot*8]. Risen 3 calls the update
// directly (not a vtable slot: hooked inline): sub rsp, 0x28; cmp byte [enabled], 0; je; mov rcx, [rcx+owner];
// test rcx, rcx; je; call <update>.
constexpr const char* kProcessCallbackPattern = "48 8B 49 ?? 48 85 C9 0F 85 ?? ?? ?? ??";
constexpr const char* kProcessSchedulePattern = "48 8B 02 4C 8B CA 4C 8B C1 48 8D 15 ?? ?? ?? ?? 49 8B C9 48 FF 60 08";
constexpr const char* kUpdateThunkPattern = "48 8B 01 48 FF A0 ?? ?? ?? ??";
// Risen 3's game mode mask (a global: 0 in gameplay, 0x20 in the inventory/map/character menu, 1 in the pause menu),
// found by its setter: cmp ecx, [mode]; je; mov [mode], ecx; mov [next], ecx; ret. Not in ELEX's code.
constexpr const char* kGameModeSetterPattern = "3B 0D ?? ?? ?? ?? 74 ?? 89 0D ?? ?? ?? ?? 89 0D ?? ?? ?? ?? F3 C3";
constexpr const char* kProcessDirectPattern = "48 83 EC 28 80 3D ?? ?? ?? ?? 00 74 ?? 48 8B 49 ?? 48 85 C9 74 ?? E8 ?? ?? ?? ??";
// ELEX 2's aim mode, gCCombatSystem2_PS::IsInAimMode (behind the script function of that name): mov rax, [rcx+aim];
// test rax, rax; je; cmp dword [rax+state], 0; jne <yes>; xorps xmm0, xmm0; comiss xmm0, [rax+cool-down]; jae <no>;
// mov al, 1; ret; xor al, al; ret. Aiming: the aim object's state isn't 0 (1 aiming, 2 shooting), or the cool-down after
// a shot still runs. Not in ELEX's code (its aim has a camera of its own).
constexpr const char* kAimModePattern =
    "48 8B 81 ?? ?? ?? ?? 48 85 C0 74 ?? 83 78 ?? 00 75 ?? 0F 57 C0 0F 2F 80 ?? ?? ?? ?? 73 ?? B0 01 C3 32 C0 C3";

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
uintptr_t g_look = 0;       // gCCharacterMovement2_PS: the object holding the heading (0: the property set itself)
uintptr_t g_heading = 0;    // that object: the heading, float[3]
uintptr_t g_turn = 0;       // the turn struct: the turn (radians, + = right)
constexpr ULONGLONG kTurnInputMs = 250;  // the gyro's yaw goes with the game's turn while that ran this recently

using ApplyTurnFn = void* (*)(void* movement, void* turn_struct, float dt);
using AngleFn = float (*)(const float* a, const float* b);
ApplyTurnFn g_apply_orig = nullptr;
AngleFn g_angle = nullptr;
std::atomic<uintptr_t> g_player_movement{0};  // the player's gCCharacterMovement2_PS
std::atomic<int64_t> g_pending_nano{0};       // the gyro's yaw waiting for the next turn, nano-degrees
std::atomic<ULONGLONG> g_turn_seen{0};         // GetTickCount64 of the last turn applied for the player
std::atomic<uint32_t> g_spurious{0};          // for the status line: spurious turns dropped

// The heading's address in a movement property set: in the object it points to at g_look (ELEX), or in the set itself
// (Risen 3, g_look 0). 0 if not readable.
uintptr_t heading_in(uintptr_t ps) {
    if (!ps || !g_heading) return 0;
    if (!g_look) return ps + g_heading;
    const uintptr_t look = mem::rdp(ps + g_look);
    return look ? look + g_heading : 0;
}

// The no-input turn's artifact (see the top of the file): a positive turn equal to the heading's angle to itself.
bool spurious_turn(uintptr_t ps, float turn) {
    if (!g_angle || !(turn > 0)) return false;
    float d[3];
    const uintptr_t at = heading_in(ps);
    if (!at || !mem::rd(at, d)) return false;
    return g_angle(d, d) == turn;
}

// The heading turned about the vertical by `rad` (+ = right), kept a horizontal unit vector.
void turn_heading(uintptr_t ps, double rad) {
    const uintptr_t at = heading_in(ps);
    float d[3];
    if (!at || !mem::rd(at, d)) return;
    const double c = std::cos(rad), s = std::sin(rad);
    const double x = c * d[0] + s * d[2], z = -s * d[0] + c * d[2];
    const double len = std::sqrt(x * x + z * z);
    if (!(len > 0.5)) return;
    const float out[3] = {float(x / len), 0.0f, float(z / len)};
    mem::guarded_store(at, out, sizeof out);
}

void* hooked_apply_turn(void* movement, void* turn_struct, float dt) {
    const auto ps = reinterpret_cast<uintptr_t>(movement);
    const auto ts = reinterpret_cast<uintptr_t>(turn_struct);
    float turn = 0;
    if (!ps || !ts || ps != g_player_movement.load(std::memory_order_relaxed) || !mem::rd(ts + g_turn, turn) ||
        !std::isfinite(turn))
        return g_apply_orig(movement, turn_struct, dt);
    g_turn_seen.store(GetTickCount64(), std::memory_order_relaxed);
    if (spurious_turn(ps, turn)) {
        g_spurious.fetch_add(1, std::memory_order_relaxed);
        turn = 0;
        mem::guarded_store(ts + g_turn, &turn, sizeof turn);
    }
    void* result = g_apply_orig(movement, turn_struct, dt);
    // The gyro's yaw, after the game's own turn, in the same call: not added to the turn struct, whose clamp (factor
    // >= 0: a turn past the body's remaining turn is cut to remaining x factor) cut it (ELEX 1 aiming; ELEX 2 always,
    // factor 10 with the stick turning: a stutter).
    if (const int64_t take = g_pending_nano.exchange(0, std::memory_order_acq_rel))
        turn_heading(ps, double(take) * 1e-9 * kPi / 180);
    return result;
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

    // <root>\system\<game>.exe with <root>\data\ini\ConfigDefault.xml and <root>\data\packed; or Risen's layout.
    bool detect(const std::wstring& exe_path) const override {
        if (risen1::detect(exe_path)) return true;
        const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
        const size_t up = dir.find_last_of(L"\\/");
        if (up == std::wstring::npos || _wcsicmp(dir.c_str() + up + 1, L"system") != 0) return false;
        const std::wstring root = dir.substr(0, up);
        return exists(root + L"\\data\\ini\\ConfigDefault.xml") && exists(root + L"\\data\\packed");
    }

    // Risen's engine is split into DLLs (Engine.dll: XInput, D3D9, the message pump); the other games are one exe.
    std::wstring module_prefix(const std::wstring&) const override { return risen1_exe() ? risen1::module_prefix() : L""; }

    void configure() override {
        risen1_ = risen1_exe();
        if (risen1_) {
            risen1::configure();
            return;
        }
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
        ui_module_vtable_ = rtti::primary_vtable(im, ".?AVgCUIModule@@");
        ui_system_vtable_ = rtti::primary_vtable(im, ".?AVgCUISystem@@");
        ui_state_vtable_ = rtti::primary_vtable(im, ".?AVgCUIState@@");
        combat_vtable_ = rtti::primary_vtable(im, ".?AVgCCombatSystem2_PS@@");
        log("genome: vtables gCCameraAdmin %s, gCCameraTargetMovement %s, gCCharacterMovement2_PS %s, eCCameraBase %s",
            admin_vtable_ ? mem::module_offset(admin_vtable_).c_str() : "not found",
            target_vtable_ ? mem::module_offset(target_vtable_).c_str() : "not found",
            movement_vtable_ ? mem::module_offset(movement_vtable_).c_str() : "not found",
            render_vtable_ ? mem::module_offset(render_vtable_).c_str() : "not found");
        if (steam_drm()) {
            code_pending_ = true;
            log("genome: the exe's code is encrypted until it starts (Steam DRM): read on the first frame");
        } else {
            read_code(im);
        }
        load_zoom_reference();
    }

    // The layout and the turn hook, from the game's code (see the top of the file).
    void read_code(const rtti::Image& im) {
        read_layout(im);
        hook_turn();
    }
    bool code_pending_ = false;

    void settings_changed() override {
        if (!risen1_) load_zoom_reference();
    }

    // Ranged aim: ELEX's and Risen 3's cam_ranged*; ELEX 2 aims with its gameplay camera, so there its aim mode picks it.
    std::vector<ViewInfo> views() const override {
        if (risen1_) return risen1::views();
        return {{1, "Default", 0, true}, {2, "Ranged aim", 1, true}};
    }

    EngineFrame read_frame() override {
        if (risen1_) return risen1::read_frame();
        EngineFrame f;
        if (code_pending_) {
            code_pending_ = false;
            read_code(rtti::exe_image());
        }
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
        if (ui_menu() || game_mode_menu()) view = 0;

        resolve_heading();
        g_player_movement.store(movement_, std::memory_order_relaxed);
        // ELEX 2 aims with its gameplay camera: the player's aim mode turns its view into the aim view, frame by frame.
        if (view == 1 && aim_obj_) {
            const bool aim = aiming();
            if (aim != aim_view_ && aim_logs_ < 300) {
                ++aim_logs_;
                log("genome: aim mode %s", aim ? "on: view Ranged aim" : "off: view Default");
            }
            aim_view_ = aim;
            if (aim) view = 2;
        }
        f.in_game = heading_ != 0;
        f.paused = !updating_;
        const bool live = updating_ && view && heading_;
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

    // Degrees from GyroLib (pitch up, yaw right). Yaw: queued, turned into the heading right after the game's own turn
    // (+ = right), or, where that isn't applied for the player, the heading direction turned about the vertical. Pitch: the camera's pitch (+ looks down),
    // when its offset is known and it holds a pitch.
    bool apply(double pitch, double yaw) override {
        if (risen1_) return risen1::apply(pitch, yaw);
        if (!heading_ || !admin_) return false;
        double v[3];
        if (!read_dir(heading_, v)) return false;
        const bool via_turn =
            g_apply_orig && GetTickCount64() - g_turn_seen.load(std::memory_order_relaxed) <= kTurnInputMs;
        if (via_turn != via_turn_ && route_logs_ < 50) {
            ++route_logs_;
            log("genome: gyro yaw %s", via_turn ? "turned in after the game's own turn" : "written to the heading (no turn applied for the player)");
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
        if (risen1_) return risen1::recenter_support(detail);
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
        if (risen1_) return risen1::recenter(fraction);
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
        if (risen1_ || via_turn_) return false;  // Risen: its turn goes in as the game's input, applied a frame later
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
        if (risen1_) return risen1::status_scene();
        if (!camera_slot_) return "genome: camera layout not known in this build";
        if (!admin_) return "genome: camera admin not found yet";
        float p = 0, y = 0;
        const bool pitch = pitch_ && mem::rd(admin_ + pitch_, p);
        const bool yaw = yaw_ && mem::rd(admin_ + yaw_, y);
        double v[3] = {};
        const bool dir = heading_ && read_dir(heading_, v);
        return std::format("genome: camera \"{}\"{}{}{}, UI \"{}\", heading {} pitch {} yaw offset {} (pitch seen "
                           "{:.1f}..{:.1f})",
                           camera_name_, updating_ ? "" : " (update stopped)", gameplay_camera_ ? "" : " (not gameplay)",
                           aim_view_ ? " (aim mode)" : "",
                           ui_states_,
                           dir ? std::format("{:.1f}", heading_of(v)) : std::string("?"),
                           pitch ? std::format("{:.1f}", -p) : std::string("?"),
                           yaw ? std::format("{:.1f}", y) : std::string("?"), -max_pitch_seen_, -min_pitch_seen_);
    }

    // Since the last status line: how far the heading turned in gameplay against the gyro's yaw (the rest is the
    // player's stick or mouse, or drift: about 0 with them idle) and the spurious turns dropped.
    void status_details() override {
        if (risen1_) return risen1::status_details();
        log("status: genome: heading turned %+.1f, gyro %+.1f (%s), rest %+.1f deg; spurious turns dropped %u",
            sum_turned_, sum_gyro_, via_turn_ ? "after the game's turn" : "written", sum_turned_ - sum_gyro_,
            g_spurious.exchange(0, std::memory_order_relaxed));
        sum_turned_ = sum_gyro_ = 0;
    }

private:
    bool risen1_ = false;  // Risen: every call goes to its route (genome_risen1.cpp)
    uintptr_t admin_vtable_ = 0, target_vtable_ = 0, movement_vtable_ = 0, render_vtable_ = 0;
    // The layout read from the code (see the top of the file); 0 (or -1) = not known.
    uintptr_t camera_slot_ = 0;   // gCCameraAdmin: the active camera's struct (its name first)
    uintptr_t pitch_ = 0, yaw_ = 0;  // gCCameraAdmin: the camera state's pitch and yaw offset
    uintptr_t admin_target_ = 0;     // gCCameraAdmin: the camera target (gCCameraTargetMovement*, checked by its vtable)
    uintptr_t target_proxy_ = 0;     // camera target -> eCEntityProxy
    int proxy_handle_ = -1;          // eCEntityProxy -> the handle (0 in ELEX 2)
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

        // The camera struct, its state's pitch and yaw offset and the camera target, from the camera frame builder.
        const char* shape = nullptr;
        for (const FrameShape& s : kFrameShapes) {
            const auto frames = find_code(s.pattern);
            if (frames.size() != 1) continue;
            CameraLayout l;
            s.read(frames[0], l);
            const auto small = [](int32_t v, int32_t max) { return v > 0 && v < max; };
            if (!small(l.slot, 0x1000) || !small(l.state, 0x1000) || !small(l.pitch, 0x400)) {
                note(std::format("camera state offsets ({} code shape, fields not readable): no gyro", s.game).c_str());
                break;
            }
            shape = s.game;
            camera_slot_ = uintptr_t(l.slot);
            pitch_ = uintptr_t(l.slot) + l.state + l.pitch;
            if (small(l.yaw, 0x400) && l.yaw != l.pitch) yaw_ = uintptr_t(l.slot) + l.state + l.yaw;
            if (small(l.target, 0x400) && l.target % 8 == 0) admin_target_ = uintptr_t(l.target);
            else note("the camera admin's target field: no gyro");
            break;
        }
        if (!camera_slot_ && missing.empty()) note("camera frame builder (no known code shape): no gyro");

        // The camera target's proxy, the movement property set's type and the property set getter's vtable slot, from
        // the camera target's vtable slot 2; the proxy's handle from the GetEntity it calls (or, virtual, eCEntityProxy's
        // slot 1).
        const uintptr_t target_fn = vtable_fn(target_vtable_, 2 * 8);
        uintptr_t proxy_at = 0;
        for (const char* p : kTargetProxyPatterns)
            if (!proxy_at) proxy_at = find_in(target_fn, 0x40, p);
        if (proxy_at) {
            target_proxy_ = rd_u8(proxy_at + 3);
            for (const char* p : kPropertySetCallPatterns) {
                const uintptr_t call = find_in(target_fn, 0x100, p);
                if (!call) continue;
                movement_type_ = rd_i32(call + 1);
                get_ps_offset_ = uintptr_t(rd_i32(call + 14));
                const uintptr_t get_entity = rd_u8(call - 5) == 0xE8 ? rel_target(call - 4) : 0;
                if (starts_with(get_entity, kGetEntityAt0Pattern)) {
                    proxy_handle_ = 0;
                } else if (starts_with(get_entity, kGetEntityPattern)) {
                    proxy_handle_ = rd_u8(get_entity + 3);
                } else if (starts_with(get_entity, kGetEntityThunkPattern)) {
                    const uintptr_t virt = vtable_fn(rtti::primary_vtable(im, ".?AVeCEntityProxy@@"), 1 * 8);
                    if (starts_with(virt, kGetEntityPattern)) proxy_handle_ = rd_u8(virt + 3);
                }
                break;
            }
        }
        if (!target_proxy_ || proxy_handle_ < 0 || movement_type_ < 0 || movement_type_ > 0xFF || !get_ps_offset_ ||
            get_ps_offset_ % 8 || get_ps_offset_ > 0x800) {
            note("player entity chain (camera target, proxy or property set getter): no gyro");
            target_proxy_ = get_ps_offset_ = 0;
            proxy_handle_ = movement_type_ = -1;
        }

        // The camera update's vtable slot, from the admin's CProcessor, its process callback and the thunk it calls.
        uintptr_t process = vtable_fn(rtti::primary_vtable(im, ".?AVCProcessor@gCCameraAdmin@@"), 2 * 8);
        if (starts_with(process, kProcessDirectPattern)) update_fn_ = rel_target(process + 0x17);
        if (starts_with(process, kProcessSchedulePattern)) process = rel_target(process + 0xC);
        if (starts_with(process, kProcessCallbackPattern)) {
            const uintptr_t thunk = rel_target(process + 9);
            if (starts_with(thunk, kUpdateThunkPattern) && rd_i32(thunk + 6) % 8 == 0) update_slot_ = rd_i32(thunk + 6) / 8;
        }
        if (update_slot_ < 0 && !update_fn_) note("camera update: no pause detection");

        // Risen 3's game mode (menus behind which the camera keeps running).
        if (const auto setters = find_code(kGameModeSetterPattern); setters.size() == 1) {
            const uintptr_t mode = rel_target(setters[0] + 2);
            if (rel_target(setters[0] + 10) == mode) game_mode_ = mode;
        }

        log("genome: layout from code (%s code shape): camera struct +%s (pitch +%s, yaw offset +%s, target +%s), camera "
            "target proxy +%s handle +%s, movement property set %s via vtable +%s, camera update %s%s%s",
            shape ? shape : "no", hex(camera_slot_).c_str(), hex(pitch_).c_str(), hex(yaw_).c_str(),
            hex(admin_target_).c_str(), hex(target_proxy_).c_str(),
            proxy_handle_ < 0 ? "?" : hex(uintptr_t(proxy_handle_)).c_str(),
            hex(uintptr_t(movement_type_ < 0 ? 0 : movement_type_)).c_str(), hex(get_ps_offset_).c_str(),
            update_fn_ ? mem::module_offset(update_fn_).c_str() : ("slot " + std::to_string(update_slot_)).c_str(),
            missing.empty() ? "" : "; not found: ", missing.c_str());
        if (game_mode_) log("genome: game mode mask %s (menus: not 0)", mem::module_offset(game_mode_).c_str());

        // ELEX 2's aim mode (it aims with its gameplay camera: no camera of its own to tell aiming apart).
        if (const auto aims = find_code(kAimModePattern); aims.size() == 1 && combat_vtable_) {
            const int32_t aim = rd_i32(aims[0] + 3), cool = rd_i32(aims[0] + 24);
            if (aim > 0 && aim < 0x1000 && cool > 0 && cool < 0x1000) {
                aim_obj_ = uintptr_t(aim);
                aim_state_ = rd_u8(aims[0] + 14);
                aim_cool_ = uintptr_t(cool);
                log("genome: aim mode from %s (gCCombatSystem2_PS::IsInAimMode: aim object +%s, state +%s, cool-down +%s): "
                    "aiming gives the Ranged aim view",
                    mem::module_offset(aims[0]).c_str(), hex(aim_obj_).c_str(), hex(aim_state_).c_str(), hex(aim_cool_).c_str());
            }
        }
    }
    // ELEX 2: the aim mode (read from IsInAimMode's code): gCCombatSystem2_PS -> aim object (+aim_obj_), its state and
    // cool-down; the player's combat property set found by its class.
    uintptr_t combat_vtable_ = 0, aim_obj_ = 0, aim_state_ = 0, aim_cool_ = 0;
    uintptr_t combat_ = 0;  // the player's gCCombatSystem2_PS
    int combat_type_ = -1;
    ULONGLONG next_combat_search_ = 0;
    bool combat_search_logged_ = false;
    bool aim_view_ = false;
    int aim_logs_ = 0;

    // The player's aim mode, read as the game's IsInAimMode reads it.
    bool aiming() const {
        const uintptr_t aim = combat_ && aim_obj_ ? mem::rdp(combat_ + aim_obj_) : 0;
        int32_t state = 0;
        float cool = 0;
        return aim && mem::rd(aim + aim_state_, state) && (state != 0 || (mem::rd(aim + aim_cool_, cool) && cool > 0));
    }

    // The player entity's gCCombatSystem2_PS: the type whose set is of that class, looked for at most once a second
    // until found (the entity may still be loading).
    uintptr_t combat_ps(uintptr_t table, uintptr_t array) {
        const auto is_combat = [&](uintptr_t ps) {
            uintptr_t vt = 0;
            return ps && mem::rd(ps, vt) && vt == combat_vtable_;
        };
        if (const uintptr_t ps = property_set(table, array, combat_type_); is_combat(ps)) return ps;
        const ULONGLONG now = GetTickCount64();
        if (now < next_combat_search_) return 0;
        next_combat_search_ = now + 1000;
        for (int type = 0; type < 0xFF; ++type) {
            const uintptr_t ps = property_set(table, array, type);
            if (!is_combat(ps)) continue;
            log("genome: the player's combat property set is type %s (its aim mode picks the aim view)",
                hex(uintptr_t(type)).c_str());
            combat_type_ = type;
            return ps;
        }
        if (!combat_search_logged_) log("genome: no combat property set among the player entity's yet (no aim view)");
        combat_search_logged_ = true;
        return 0;
    }

    uintptr_t game_mode_ = 0;  // Risen 3: the game mode mask (0: gameplay)
    uint32_t last_game_mode_ = 0;
    int game_mode_logs_ = 0;
    int update_slot_ = -1;     // the camera update's slot in gCCameraAdmin's vtable
    uintptr_t update_fn_ = 0;  // or the update function the processor calls directly (Risen 3: hooked inline)

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
    // its owner. Looked for once a second until found; then its camera update is hooked (vtable slot, or inline).
    bool find_admin() {
        const ULONGLONG now = GetTickCount64();
        if (!admin_vtable_ || now < next_search_) return false;
        next_search_ = now + 1000;
        for (const mem::Range& r : mem::writable_sections()) {
            for (uintptr_t p = r.begin; p + kAdminOwner + 8 <= r.end; p += 8) {
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
        void* original = nullptr;
        if (update_fn_) {
            if (!hooks::hook_function(reinterpret_cast<void*>(update_fn_), reinterpret_cast<void*>(&hooked_update),
                                      &original))
                original = nullptr;
        } else if (update_slot_ >= 0) {
            original = rtti::patch_slot(admin_vtable_, update_slot_, reinterpret_cast<void*>(&hooked_update));
        }
        g_update_orig = reinterpret_cast<UpdateFn>(original);
        log("genome: camera admin %s; camera update (%s) %s", mem::module_offset(admin_).c_str(),
            update_fn_ ? ("inline, " + mem::module_offset(update_fn_)).c_str() : ("slot " + std::to_string(update_slot_)).c_str(),
            original ? ("hooked, was " + mem::module_offset(reinterpret_cast<uintptr_t>(original))).c_str()
                     : "NOT hooked (no pause detection)");
        return true;
    }

    // --- the UI's state stack (menus) ---------------------------------------------------------------------------------
    // The static gCUIModule points to its gCUISystem, which keeps the UI states shown as a stack (array of gCUIState*,
    // count): "Game" in gameplay, "PauseMenu" etc. on top of it in menus (ELEX 2 keeps the gameplay camera behind its
    // inventory, map and skill menus, so the camera can't tell them apart). The module is found by its vtable in the
    // exe's data, the system and the stack by their classes (offsets read once, from the live objects). A state's name:
    // char* at +0x10, checked printable.
    static constexpr uintptr_t kUiStateName = 0x10;
    uintptr_t ui_module_vtable_ = 0, ui_system_vtable_ = 0, ui_state_vtable_ = 0;
    uintptr_t ui_module_ = 0, ui_system_off_ = 0, ui_stack_off_ = 0;
    ULONGLONG next_ui_search_ = 0;
    std::string ui_states_;  // "Game > PauseMenu", last seen
    int ui_logs_ = 0;
    bool ui_search_logged_ = false;

    bool is_ui_state(uintptr_t o) const {
        uintptr_t vt = 0;
        return o && mem::plausible(o) && mem::rd(o, vt) && vt == ui_state_vtable_;
    }

    bool find_ui() {
        if (ui_stack_off_) return true;
        const ULONGLONG now = GetTickCount64();
        if (!ui_module_vtable_ || !ui_system_vtable_ || !ui_state_vtable_ || now < next_ui_search_) return false;
        next_ui_search_ = now + 1000;
        if (!ui_module_) {
            for (const mem::Range& r : mem::writable_sections()) {
                for (uintptr_t p = r.begin; p + 8 <= r.end && !ui_module_; p += 8) {
                    uintptr_t v = 0;
                    if (mem::rd(p, v) && v == ui_module_vtable_) ui_module_ = p;
                }
                if (ui_module_) break;
            }
            if (!ui_module_) return false;
        }
        uintptr_t system = 0;
        for (uintptr_t off = 8; off < 0x200 && !system; off += 8) {
            const uintptr_t o = mem::rdp(ui_module_ + off);
            uintptr_t vt = 0;
            if (o && mem::rd(o, vt) && vt == ui_system_vtable_) system = o, ui_system_off_ = off;
        }
        if (!system) return false;
        for (uintptr_t off = 8; off < 0x100; off += 8) {
            const uintptr_t array = mem::rdp(system + off);
            uint32_t count = 0, capacity = 0;
            if (!array || !mem::rd(system + off + 8, count) || !mem::rd(system + off + 12, capacity) || count < 1 ||
                count > 32 || capacity < count || capacity > 1024)
                continue;
            bool states = true;
            for (uint32_t i = 0; i < count && states; ++i) states = is_ui_state(mem::rdp(array + 8 * i));
            if (!states) continue;
            ui_stack_off_ = off;
            break;
        }
        if (ui_stack_off_ || !ui_search_logged_)
            log("genome: UI module %s: UI system +%s, its state stack %s", mem::module_offset(ui_module_).c_str(),
                hex(ui_system_off_).c_str(), ui_stack_off_ ? ("+" + hex(ui_stack_off_)).c_str() : "not found yet");
        ui_search_logged_ = true;
        return ui_stack_off_ != 0;
    }

    // The UI's state names, bottom to top ("" if not readable).
    std::vector<std::string> ui_states() {
        std::vector<std::string> out;
        if (!find_ui()) return out;
        const uintptr_t system = mem::rdp(ui_module_ + ui_system_off_);
        const uintptr_t array = system ? mem::rdp(system + ui_stack_off_) : 0;
        uint32_t count = 0;
        if (!array || !mem::rd(system + ui_stack_off_ + 8, count) || count > 32) return out;
        for (uint32_t i = 0; i < count; ++i) {
            const uintptr_t o = mem::rdp(array + 8 * i);
            char buf[40] = {};
            const uintptr_t s = is_ui_state(o) ? mem::rdp(o + kUiStateName, 1) : 0;
            if (!s || !mem::guarded_copy(buf, s, sizeof buf - 1)) return {};
            out.emplace_back(buf, strnlen(buf, sizeof buf - 1));
        }
        return out;
    }

    // Risen 3: a menu while the game mode mask isn't 0 (each change logged).
    bool game_mode_menu() {
        uint32_t mode = 0;
        if (!game_mode_ || !mem::rd(game_mode_, mode)) return false;
        if (mode != last_game_mode_ && game_mode_logs_ < 300) {
            ++game_mode_logs_;
            log("genome: game mode %s%s", hex(mode).c_str(), mode ? " (menu: gyro off)" : " (gameplay)");
        }
        last_game_mode_ = mode;
        return mode != 0;
    }

    // A menu: a UI state other than "Game" on top of a stack that starts with "Game" (the gameplay state; a stack named
    // otherwise isn't read as menus: the camera's name still is).
    bool ui_menu() {
        const auto states = ui_states();
        std::string joined;
        for (const auto& s : states) joined += (joined.empty() ? "" : " > ") + s;
        if (joined != ui_states_ && ui_logs_ < 300) {
            ++ui_logs_;
            log("genome: UI states %s", joined.empty() ? "(not readable)" : joined.c_str());
        }
        ui_states_ = joined;
        return !states.empty() && states.front() == "Game" && states.back() != "Game";
    }

    // The function that applies the movement's turn (main thread), by its first bytes: the movement object's offset,
    // the heading's and the turn struct's fields read from its code, then hooked to add the gyro's yaw; and the game's
    // angle function, by its call in the movement job, for the spurious-turn check.
    void hook_turn() {
        const TurnShape* shape = nullptr;
        uintptr_t fn = 0;
        for (const TurnShape& s : kTurnShapes) {
            const auto hits = find_code(s.pattern);
            if (hits.size() != 1) continue;
            shape = &s;
            fn = hits[0];
            break;
        }
        if (!shape) {
            log("genome: turn function not found (no known code shape): no gyro (the heading's offsets come from it)");
            return;
        }
        const uintptr_t look = shape->look_at ? rd_u8(fn + shape->look_at) : 0;
        const uintptr_t turn = find_in(fn, 0x400, shape->turn);
        const uintptr_t remaining = find_in(fn, 0x400, shape->remaining);
        const uintptr_t heading = find_in(fn, 0x400, shape->heading);
        if ((shape->look_at && !look) || !heading || !rd_u8(heading + 3)) {
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
        if (!hooks::hook_function(reinterpret_cast<void*>(fn), reinterpret_cast<void*>(&hooked_apply_turn),
                                  reinterpret_cast<void**>(&g_apply_orig))) {
            g_apply_orig = nullptr;
            log("genome: turn function %s: hook failed; gyro yaw written to the heading", mem::module_offset(fn).c_str());
            return;
        }
        log("genome: turn function %s (%s code shape) hooked: gyro yaw turned into the heading after the game's own turn "
            "(movement %s, heading +%s, turn +%s, remaining +%s, factor +%s)",
            mem::module_offset(fn).c_str(), shape->game, g_look ? ("+" + hex(g_look)).c_str() : "itself",
            hex(g_heading).c_str(), hex(g_turn).c_str(),
            hex(r).c_str(), hex(f).c_str());
        for (const AngleShape& s : kAngleShapes) {
            auto calls = find_code(s.pattern);
            if (s.heading_at)  // the copy of this heading, not of another vector
                std::erase_if(calls, [&](uintptr_t c) { return rd_u8(c + s.heading_at) != g_heading; });
            if (calls.size() != 1) continue;
            const uintptr_t angle = rel_target(calls[0] + s.call_at);
            if (!starts_with(angle, "40 53 48 83 EC 40")) continue;  // push rbx; sub rsp, 0x40
            g_angle = reinterpret_cast<AngleFn>(angle);
            log("genome: angle function %s (%s code shape): spurious turns (heading to itself) dropped",
                mem::module_offset(angle).c_str(), s.game);
            return;
        }
        log("genome: angle function not found (no known code shape): spurious turns kept (the camera may creep right)");
    }

    // An entity's property set of type `type` (its tables: index bytes at type + 1, pointer array), 0 if it has none.
    static uintptr_t property_set(uintptr_t table, uintptr_t array, int type) {
        uint8_t slot = 0xFF;
        if (!table || !array || type < 0 || !mem::rd(table + uintptr_t(type) + 1, slot) || slot == 0xFF) return 0;
        return mem::rdp(array + uintptr_t(slot) * 8);
    }

    // The entity's gCCharacterMovement2_PS: the type the camera target asks for (ELEX), else the type whose set is of that
    // class, looked for once (Risen 3's camera target asks for its animation controller's, 0x69; the movement is 0x68).
    uintptr_t movement_ps(uintptr_t table, uintptr_t array) {
        const auto is_movement = [&](uintptr_t ps) {
            uintptr_t vt = 0;
            return ps && mem::rd(ps, vt) && vt == movement_vtable_;
        };
        if (const uintptr_t ps = property_set(table, array, movement_type_); is_movement(ps)) return ps;
        if (movement_searched_) return 0;
        movement_searched_ = true;
        for (int type = 0; type < 0xFF; ++type) {
            const uintptr_t ps = property_set(table, array, type);
            if (!is_movement(ps)) continue;
            log("genome: the player's movement property set is type %s (the camera target asks for %s)",
                hex(uintptr_t(type)).c_str(), hex(uintptr_t(movement_type_)).c_str());
            movement_type_ = type;
            return ps;
        }
        log("genome: no movement property set among the player entity's (no gyro)");
        return 0;
    }
    bool movement_searched_ = false;

    // The player's heading direction, from the camera target (every frame: the target and the entity can change).
    void resolve_heading() {
        uintptr_t found = 0, movement = 0, combat = 0;
        const char* why = nullptr;
        uintptr_t target = 0, vt = 0, index = 0, array = 0;
        if (!g_heading || !target_proxy_ || !admin_target_) {
            why = "the heading's offsets aren't known in this build";
        } else if (!mem::rd(admin_ + admin_target_, target) || !mem::plausible(target) || !mem::rd(target, vt) ||
                   vt != target_vtable_) {
            why = "no player camera target";
        } else {
            const uintptr_t handle = mem::rdp(target + target_proxy_ + uintptr_t(proxy_handle_));
            const uintptr_t entity = handle ? mem::rdp(handle) : 0;
            const uintptr_t table = entity && ps_tables(entity, index, array) ? mem::rdp(entity + index, 1) : 0;
            const uintptr_t ps = table ? movement_ps(table, mem::rdp(entity + array)) : 0;
            if (aim_obj_ && table) combat = combat_ps(table, mem::rdp(entity + array));
            if (!ps) {
                why = "no player entity or movement";
            } else {
                const uintptr_t at = heading_in(ps);
                double v[3];
                if (!at) why = "no movement property set";
                else if (!read_dir(at, v)) why = "heading not a horizontal unit vector";
                else found = at, movement = ps;
            }
        }
        if (found != heading_ && heading_logs_ < 50) {
            ++heading_logs_;
            log("genome: heading direction %s", found ? mem::module_offset(found).c_str() : why);
        }
        heading_ = found;
        movement_ = movement;
        combat_ = combat;
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
