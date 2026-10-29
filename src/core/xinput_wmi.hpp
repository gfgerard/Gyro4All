#pragma once
// The XInput device check of Microsoft's IsXInputDevice sample (WMI DeviceID with "IG_"), passed by every HID game
// controller the XInput translation may show (xinput_wmi.cpp).
namespace gyro4all::xinput {

// DllMain, from xinput::install (XInput=1 or 2): hooks the game's CoCreateInstance. sony_too: PlayStation controllers
// too (XInput=2; with XInput=1 they are left out when the game reads them itself).
void install_wmi(bool sony_too);

}  // namespace gyro4all::xinput
