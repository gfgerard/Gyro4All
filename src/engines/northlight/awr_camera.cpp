// Alan Wake Remastered's gameplay camera (MODLOG 2026-10-30, found with camtrace in the Epic build).
//
// The player's look is a plain struct (no vtable; the global exe+0xb20980 in that build points at it): eye and target
// positions at +0x00 and +0x0c, the look direction as a unit float 3-vector at +0x18 (world +Y up, heading atan2(x, z), as
// Control's and Alan Wake 2's), and its yaw and pitch in radians at +0x24 and +0x28, derived from the direction.
// PlayerInputReader's virtual method at vtable+0xc8 (exe+0x29f1b0; rcx the reader, the look struct at its +0x50, rdx a
// 3x3 rotation) turns the direction by the rotation and stores it with the angles, every frame while the player
// controls the camera. TailCamera (the over-the-shoulder camera) builds its view from that direction (plus a shoulder
// offset and smoothing), so a turn written into the direction before the method runs is kept: the hook adds the gyro's
// turn there (LookQueue: an even share per run).
//
// Found by a run of its code (unique in the exe) and the function around it in the unwind table.
#include "awr_camera.hpp"

#include "hooks.hpp"
#include "log.hpp"
#include "look_queue.hpp"
#include "mem.hpp"

#include <windows.h>

#include <atomic>

namespace gyro4all::northlight::awr {

namespace {

// mov rbx,rdx; movaps [rsp+30h],xmm6; mov rdi,rcx; lea rdx,[rsp+20h]; mov rcx,[rcx+50h]; call: the method reading the
// look direction of the struct at its +0x50.
constexpr uint8_t kTurnCode[] = {0x48, 0x8B, 0xDA, 0x0F, 0x29, 0x74, 0x24, 0x30, 0x48, 0x8B, 0xF9,
                                 0x48, 0x8D, 0x54, 0x24, 0x20, 0x48, 0x8B, 0x49, 0x50, 0xE8};
constexpr uintptr_t kLookOffset = 0x50;  // the look struct in the reader
constexpr uintptr_t kDirection = 0x18;   // the look direction in the look struct
// Gone this long, the camera isn't running (menus, loading, cutscenes).
constexpr ULONGLONG kIdleMs = 100;

using Turn_t = void (*)(uintptr_t, uintptr_t);
Turn_t g_turn = nullptr;
std::atomic<bool> g_hooked{false};
std::atomic<uintptr_t> g_live{0};  // the look direction the method turned last
std::atomic<ULONGLONG> g_last_run{0};
LookQueue g_queue;

void hk_turn(uintptr_t reader, uintptr_t rotation) {
    const uintptr_t look = mem::rdp(reader + kLookOffset);
    if (look) {
        const uintptr_t dir = look + kDirection;
        if (g_live.exchange(dir) != dir) log("northlight: look direction %p", reinterpret_cast<void*>(dir));
        g_last_run = GetTickCount64();
        double pitch, yaw, up, down;
        if (g_queue.take(pitch, yaw, up, down)) turn_look(dir, pitch, yaw, up, down);
    }
    g_turn(reader, rotation);
}

}  // namespace

bool install() {
    int hits = 0;
    const uintptr_t at = find_unique_code(kTurnCode, sizeof kTurnCode, SIZE_MAX, hits);
    const uintptr_t turn = at ? function_start(at) : 0;
    if (!turn) {
        log("northlight: Alan Wake Remastered's look turn not found (its code matched %d times): gyro as right stick",
            hits);
        return false;
    }
    if (!hooks::hook_function(reinterpret_cast<void*>(turn), reinterpret_cast<void*>(&hk_turn),
                              reinterpret_cast<void**>(&g_turn))) {
        log("northlight: Alan Wake Remastered's look turn at %s could not be hooked: gyro as right stick",
            mem::module_offset(turn).c_str());
        return false;
    }
    g_hooked = true;
    log("northlight: Alan Wake Remastered's look turn hooked at %s: the gyro turns the camera in the game's own update",
        mem::module_offset(turn).c_str());
    return true;
}

bool hooked() { return g_hooked; }

uintptr_t live() {
    const uintptr_t d = g_live.load();
    return d && GetTickCount64() - g_last_run.load() <= kIdleMs ? d : 0;
}

bool look(double& pitch, double& heading) {
    const uintptr_t d = live();
    return d && look_angles(d, pitch, heading);
}

void add(double pitch, double yaw, double max_up, double max_down) { g_queue.add(pitch, yaw, max_up, max_down); }

double pending_pitch() { return g_queue.pending_pitch(); }

}  // namespace gyro4all::northlight::awr
