#pragma once
// Alan Wake Remastered's gameplay camera (awr_camera.cpp): the look direction the player's input reader turns each frame,
// turned by the gyro in a hook on that turn.
#include <cstdint>

namespace gyro4all::northlight::awr {

// DllMain (configure): finds the look turn in the exe and hooks it. False when its code isn't found (another build) or
// can't be hooked: the gyro then goes to the game as right stick.
bool install();
// The look turn is hooked.
bool hooked();
// The look direction's address while the turn runs (0 when it hasn't for 100 ms: menus, loading, cutscenes).
uintptr_t live();
// The live look's pitch (degrees, up positive) and heading (degrees, grows turning right).
bool look(double& pitch, double& heading);
// Gyro4All's frame: adds a turn (degrees) for the hook to apply on its next run, within the pitch limits.
void add(double pitch, double yaw, double max_up, double max_down);
// The pitch (degrees) added and not applied yet.
double pending_pitch();

}  // namespace gyro4all::northlight::awr
