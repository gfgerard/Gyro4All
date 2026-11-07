#pragma once

namespace gyro4all {

class Engine;

// Called once from DllMain, after settings::configure and hooks::init, with the engine chosen for this game.
void host_configure(Engine& engine);

// Called on the game thread once per message pump (≈ once per frame), outside any engine lock.
// Returns true when the gyro applied a camera rotation this frame.
bool host_frame();

// A WM_INPUT message the game's thread received (before the game handles it): relative mouse motion counts as the
// player turning the camera, like the right stick (camera probe, camera checks). Reads it only, never consumes it.
void host_raw_input(void* raw_input_handle);

namespace host {

// For engine modules, on the game thread.
bool game_has_focus();
// The right stick is deflected (GyroLib's view of the selected controller, SDL or a bound companion). available (if
// given) is set to true when GyroLib reports the stick at all.
bool right_stick_moving(bool* available = nullptr);
// Relative mouse motion (Raw Input reaching the game's main thread) in the last 150 ms.
bool mouse_moving();
// Gyro4All's or GyroLib's panel is open: its stick and mouse aren't the player's look input.
bool panel_open();
// The game doesn't see the right stick now (the stick camera or flick stick has it): moving it can't turn the game's camera.
bool stick_hidden();
// The core's stick fallback gives the gyro's turn to the game as right-stick input.
bool stick_fallback_active();

}  // namespace host
}  // namespace gyro4all
