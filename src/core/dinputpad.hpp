#pragma once
// The game's DirectInput game controllers, in every game (import-table and inline-hook mode):
// - GyroLib's settings panel: while it has the controller, the game's GetDeviceState gets the state it last read while
//   the controller was at rest (its own data format and axis ranges, so no layout knowledge is needed), and
//   GetDeviceData no buffered events. RE2 (Xbox build) reads the DualSense this way, below anything the HID hooks see.
// - Diagnostics: each place the game reads a controller from (call site, buffer, data format) is logged once, and the
//   last GetDeviceState buffer is WriteWatch's "di" address (writewatch.hpp), to follow the stick into the game's code.
// - XInput auto mode (xinput.cpp): reading a game controller through DirectInput is a sign the game reads controllers
//   itself (creating DirectInput or devices is not: RE7, RE8 and MH Wilds do that only to identify controllers).
// - The settings panel: the game's DirectInput mice and keyboards read idle while the panel captures them (set_capture).
// Devices created by Gyro4All, gyrolib.dll or its SDL3.dll (SDL has a DirectInput backend) are left alone.
#include <cstdint>

struct SDL_Gamepad;

namespace gyro4all::dinputpad {

// DllMain: DirectInput8Create (import slots of the game modules, or dinput8.dll's export inline in protected exes),
// then the vtables of the interfaces it creates (CreateDevice; GetDeviceState, GetDeviceData of game controllers).
void install();

// Called the first time the game reads each of its DirectInput game controllers (any thread). id: vendor << 16 |
// product (0 unknown); how: "GetDeviceState" or "GetDeviceData".
using ReadFn = void (*)(uint32_t id, const char* how);
void on_read(ReadFn fn);

// Called on the game's thread each time the game calls DirectInput8Create, before the real one runs: the game's code
// is unpacked by then (protected exes) and it hasn't enumerated any device yet (an engine can hook its own device
// checks there).
using CreateFn = void (*)();
void on_create(CreateFn fn);

// Game thread, once per frame: neutral = GyroLib's panel has the controller; pad = GyroLib's selected controller (its
// rest state decides when the game's state is remembered as the idle one).
void frame(bool neutral, SDL_Gamepad* pad);

// Game thread, once per frame: the settings panel captures the mouse / the keyboard (overlay::capture). The game's
// DirectInput mice and keyboards then read nothing pressed and no motion; buffered reads keep only releases.
void set_capture(bool mouse, bool keyboard);

// Stick fallback (stick_fallback.hpp; game thread): right-stick deflection added to what the game's GetDeviceState
// returns (x right, y up, -1..1; 0, 0 = none). Standard joystick formats only, right stick on Z/Rz (PlayStation
// controllers).
void set_stick(float x, float y);

// The buffer the game passed to its last successful GetDeviceState of a game controller, and its size (any thread).
// False before the first one. The buffer may be on the game's stack (the log line of its call site says so).
bool last_state(uintptr_t& buffer, uint32_t& size);

}  // namespace gyro4all::dinputpad
