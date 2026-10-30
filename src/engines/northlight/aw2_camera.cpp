// Alan Wake 2's gameplay camera (MODLOG 2026-10-30, found with camtrace in the Epic build).
//
// The camera is an ECS component (no vtable): its state keeps the look direction as a unit float 3-vector at +0x00 (world
// +Y up, heading atan2(x, z), as Control's), the stick's yaw and pitch rates at +0x90 and +0x98, the angles at +0x8c
// (yaw) and +0x94 (pitch). Its update (exe+0x191a4b0 in that build; 8 arguments, a float frame time 5th, the state 6th)
// reads the direction, turns it by the rates times the frame time, wraps the yaw, clamps the pitch to the game's limits
// and writes the direction back, every frame, on a worker thread of the game's job system. A turn written into the
// direction before it runs is kept, so the hook adds the gyro's turn there (LookQueue: an even share per update).
//
// Found by a run of its code (unique in the exe) and the function around it in the unwind table.
#include "aw2_camera.hpp"

#include "hooks.hpp"
#include "log.hpp"
#include "look_queue.hpp"
#include "mem.hpp"

#include <windows.h>

#include <atomic>

namespace gyro4all::northlight::aw2 {

namespace {

// mov rbx,rcx; mov rsi,[rbp+6Fh]; vmovups xmm0,[rsi]; vmovsd [rsp+40h],xmm0: the update loading the direction from its
// 6th argument.
constexpr uint8_t kLoadCode[] = {0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x75, 0x6F, 0xC5, 0xF8, 0x10,
                                 0x06, 0xC5, 0xFB, 0x11, 0x44, 0x24, 0x40};
// Gone this long, the camera isn't running (menus, loading, cutscenes).
constexpr ULONGLONG kIdleMs = 100;

using Update_t = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, float, uintptr_t, uintptr_t, uintptr_t);
Update_t g_update = nullptr;
std::atomic<bool> g_hooked{false};
std::atomic<uintptr_t> g_live{0};  // the state the update ran on last
std::atomic<ULONGLONG> g_last_run{0};
std::atomic<uintptr_t> g_other{0};  // the last other state seen while the live one ran (logged once)
LookQueue g_queue;

uintptr_t hk_update(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, float dt, uintptr_t state, uintptr_t a7,
                    uintptr_t a8) {
    const ULONGLONG now = GetTickCount64();
    uintptr_t live = g_live.load(std::memory_order_relaxed);
    // Another state takes over once the live one has stopped (a respawn, the other character); while both run, the
    // first one stays.
    if (state != live) {
        if (!live || now - g_last_run.load(std::memory_order_relaxed) > kIdleMs) {
            g_live = live = state;
            log("northlight: camera state %p", reinterpret_cast<void*>(state));
        } else if (g_other.exchange(state) != state) {
            log("northlight: another camera state %p runs too (not turned)", reinterpret_cast<void*>(state));
        }
    }
    if (state == live) {
        g_last_run = now;
        double pitch, yaw, up, down;
        if (g_queue.take(pitch, yaw, up, down, dt)) turn_look(state, pitch, yaw, up, down);
    }
    return g_update(a1, a2, a3, a4, dt, state, a7, a8);
}

}  // namespace

bool install() {
    int hits = 0;
    const uintptr_t at = find_unique_code(kLoadCode, sizeof kLoadCode, SIZE_MAX, hits);
    const uintptr_t update = at ? function_start(at) : 0;
    if (!update) {
        log("northlight: Alan Wake 2's camera update not found (its code matched %d times): gyro as right stick", hits);
        return false;
    }
    if (!hooks::hook_function(reinterpret_cast<void*>(update), reinterpret_cast<void*>(&hk_update),
                              reinterpret_cast<void**>(&g_update))) {
        log("northlight: Alan Wake 2's camera update at %s could not be hooked: gyro as right stick",
            mem::module_offset(update).c_str());
        return false;
    }
    g_hooked = true;
    log("northlight: Alan Wake 2's camera update hooked at %s: the gyro turns the camera in the game's own update",
        mem::module_offset(update).c_str());
    return true;
}

bool hooked() { return g_hooked; }

uintptr_t live() {
    const uintptr_t s = g_live.load();
    return s && GetTickCount64() - g_last_run.load() <= kIdleMs ? s : 0;
}

bool look(double& pitch, double& heading) {
    const uintptr_t s = live();
    return s && look_angles(s, pitch, heading);
}

void add(double pitch, double yaw, double max_up, double max_down) { g_queue.add(pitch, yaw, max_up, max_down); }

double pending_pitch() { return g_queue.pending_pitch(); }

}  // namespace gyro4all::northlight::aw2
