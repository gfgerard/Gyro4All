#pragma once
// GyroLib's long-press blocking (activation.block_long_press): with Hold or Hold to disable on a single button, a
// short press of that button still does its game action (on release), a hold only drives the gyro. Gyro4All routes the
// selected controller's button events through gl_filter_event and applies the result to what the game reads: the
// controller shown as XInput, a real XInput controller, and the reports of the game's own PlayStation support.
// Button bits are SDL_GamepadButton ordinals (bit n = button n), like GyroLib's.
#include <cstdint>

struct gl_context;
struct SDL_Gamepad;

namespace gyro4all::longpress {

// What the game reads for each button: (pressed && !(hide & bit)) || (tap & bit).
struct Mask {
    uint32_t hide = 0;  // held back from the game (deferred or blocked press)
    uint32_t tap = 0;   // a short press the game gets now (a tap released before 200 ms)
};

// Game thread, once per frame after gl_update. pad = GyroLib's selected controller; active = the game has focus and
// the panel doesn't have the controller; declared = GL_HOST_LONG_PRESS_BLOCKING is declared. While inactive, held
// buttons are finished without taps and pending taps are dropped.
Mask frame(gl_context* ctx, SDL_Gamepad* pad, uint64_t now_ns, bool active, bool declared);

}  // namespace gyro4all::longpress
