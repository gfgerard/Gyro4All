#pragma once
// The game's own access to PlayStation controllers (Sony's PC pad library, as libScePad.dll or linked into the exe,
// opens the controller's HID device with CreateFileW and reads its input reports with ReadFile /
// GetOverlappedResult). Used for:
// - XInput=2: refusing those opens, so the game only gets the Xbox controller (xinput.cpp);
// - auto mode: knowing which controllers the game opens itself (xinput.cpp);
// - GyroLib's settings panel: while the pad drives it, the game reads an idle controller;
// - GyroLib's long-press blocking: the activation button's press held back or added as a short tap;
// - GyroLib's flick stick with the game's native controller support: while GyroLib asks to suppress the native right
//   stick, the right stick in the reports the game reads is centred (the controller itself, and GyroLib's own SDL
//   reading, are untouched). Offered only once the report layout was checked against SDL's view of the stick.
#include <cstdint>

struct SDL_Gamepad;

namespace gyro4all::nativepad {

// DllMain (after hooks::init): CreateFileW in the game modules and a separate libScePad*.dll; with suppression also
// ReadFile, GetOverlappedResult(Ex) and CloseHandle. Protected exes (inline-hook mode): inline hooks on those system
// exports, following the game's opens (not GyroLib's), without the DualShock 4 identity.
// dualsense_as_ds4: a DualSense (USB or Bluetooth) is shown to the game's own PlayStation support as a USB DualShock 4
// (identity, caps, feature reports, input and output reports converted), for games whose Sony library predates the
// DualSense.
// dualsense_as_ds4: the game's profile says its PlayStation support only knows the DualShock 4 (written there by the
// host when dualsense_ignored() was seen in an earlier session, or shipped with the profile). learned: the host wrote
// it (DualSenseAsDualShock4Learned=1 next to it), so it may take it back (learned_ds4_rejected).
void install(bool suppression, bool dualsense_as_ds4, bool learned);

// The game opened a DualSense, closed it again unread and has none open 5 s later: its own PlayStation support doesn't
// take the DualSense (only the DualShock 4). False with dualsense_as_ds4 or once the game read a DualSense.
bool dualsense_ignored();

// The same with the DualShock 4 identity Gyro4All learned itself: the game rejected the emulation too (RE7 finds its
// DualShock 4 through DirectInput, which still reports a DualSense). The controller is then shown as an Xbox one
// for the session, whatever the auto mode's native signs, and the host writes DualSenseAsDualShock4=0.
bool learned_ds4_rejected();

// XInput=2: refuse the game's opens of Sony/Nintendo HID devices and of the selected controller.
void refuse_pads(bool refuse);

// Game thread, once per frame: while GyroLib's panel has the controller, every report of a Sony controller the game
// reads (known layout, CRC checked) reads as an idle controller: no buttons, centred sticks, triggers up.
void set_neutral(bool neutral);

// Game thread, once per frame: long-press blocking's button mask (longpress.cpp, SDL button bits) for the selected
// controller's reports. Applied, and true returned, only while the game reads that controller itself and its
// button bytes were checked against SDL (frame() checks them whenever a button is down).
bool set_buttons(uint32_t hide, uint32_t tap);

// Stick fallback (stick_fallback.hpp; game thread): right-stick deflection added to the selected controller's right
// stick in the game's own reports (x right, y up, -1..1; 0, 0 = none).
void set_stick(float x, float y);

// Vendor or product ID (key "vid" / "pid") in a HID device path, USB or Bluetooth form; 0 if absent.
uint16_t hid_path_id(const wchar_t* path, const wchar_t* key);

// The game opened a HID device of this vendor itself (auto mode's native-support check).
bool opened_vendor(uint16_t vendor);

// Game thread, once per frame: pad = GyroLib's selected controller (null if none), suppress = GyroLib's
// suppress_native_right_stick for it. Returns true while the game reads that controller itself through a checked
// report layout, i.e. native right-stick suppression works (declare GL_HOST_NATIVE_STICK_SUPPRESSION then).
bool frame(SDL_Gamepad* pad, bool suppress);

}  // namespace gyro4all::nativepad
