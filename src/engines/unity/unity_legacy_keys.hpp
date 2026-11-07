// Unity's KeyCode joystick buttons as XInput buttons, in Unity's Xbox controller order on Windows. No game access.
#pragma once

#include <cstdint>

namespace gyro4all::unity {

// XINPUT_GAMEPAD_* of KeyCode.JoystickButtonN (330..349, any joystick) or KeyCode.Joystick1ButtonN (350..369); 0 = no
// such button on an Xbox controller, or another key.
inline uint16_t legacy_key_button(int key) {
    constexpr uint16_t kButtons[10] = {0x1000 /*A*/,  0x2000 /*B*/,    0x4000 /*X*/,     0x8000 /*Y*/,  0x0100 /*LB*/,
                                       0x0200 /*RB*/, 0x0020 /*Back*/, 0x0010 /*Start*/, 0x0040 /*LS*/, 0x0080 /*RS*/};
    const int n = key >= 350 && key < 370 ? key - 350 : key >= 330 && key < 350 ? key - 330 : -1;
    return n >= 0 && n < 10 ? kButtons[n] : uint16_t(0);
}

}  // namespace gyro4all::unity
