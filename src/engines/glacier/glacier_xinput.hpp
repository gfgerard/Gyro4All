#pragma once
// Glacier's XInput device check (glacier_xinput.cpp): the game makes XInput devices only of controllers that
// DirectInput lists and WMI calls XInput, so the Nintendo controller Gyro4All shows as XInput is made to pass it.
namespace gyro4all::glacier {

// From the game's DirectInput8Create (dinputpad::on_create): finds the check from the exe's "IG_" string and hooks it
// (once; nothing when the XInput translation is off).
void hook_xinput_device_check();

}  // namespace gyro4all::glacier
