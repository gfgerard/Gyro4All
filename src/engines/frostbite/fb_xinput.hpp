#pragma once
// Frostbite's XInput device check (fb_xinput.cpp): the game makes XInput devices only of controllers that DirectInput
// lists and WMI calls XInput, so the controller Gyro4All shows as XInput is made to pass it.
namespace gyro4all::fb {

// From the game's DirectInput8Create (dinputpad::on_create), game thread: finds the check from the exe's
// XInputGetCapabilities import and hooks it (once; nothing when the XInput translation is off).
void hook_xinput_device_check();

}  // namespace gyro4all::fb
