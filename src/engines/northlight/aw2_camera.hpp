#pragma once
// Alan Wake 2's gameplay camera (aw2_camera.cpp): the look direction its camera update turns from each frame, turned by
// the gyro in a hook on that update.
#include <cstdint>

namespace gyro4all::northlight::aw2 {

// DllMain (configure): finds the camera update in the exe and hooks it. False when its code isn't found (another
// build) or can't be hooked: the gyro then goes to the game as right stick.
bool install();
// The update is hooked.
bool hooked();
// The camera state the update ran on last, while it runs (0 when it hasn't for 100 ms: menus, loading, cutscenes).
uintptr_t live();
// The live camera's pitch (degrees, up positive) and heading (degrees, grows turning right).
bool look(double& pitch, double& heading);
// Gyro4All's frame: adds a turn (degrees) for the update to apply on its next run, within the pitch limits.
void add(double pitch, double yaw, double max_up, double max_down);
// The pitch (degrees) added and not applied yet.
double pending_pitch();

}  // namespace gyro4all::northlight::aw2
