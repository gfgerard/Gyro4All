#pragma once
// 007 First Light's camera, turned through the active orbit camera's angles (glacier_look.cpp, MODLOG 2026-10-29).
#include <cstddef>
#include <cstdint>
#include <string>

namespace gyro4all::glacier {

// The address in the exe's section `in` a RIP-relative instruction refers to, from the code around it; code_at: where
// the code is (glacier_engine.cpp).
uintptr_t data_from_code(const uint8_t* code, const uint8_t* mask, size_t n, const char* what, size_t disp_at = 3,
                         size_t insn_end = 7, const char* in = ".data", uintptr_t* code_at = nullptr);

namespace look {

// Any thread, once: finds the orbit camera and hooks its update. False if this build's isn't known.
bool install();
bool installed();
// Game thread: the orbit camera's update ran lately (not in the front end, a load or the pause menu).
bool live();
// Game thread: the view's angles as the game left them (degrees: yaw right, pitch up), false if unreadable.
bool view(double& pitch, double& yaw);
// Game thread: adds the gyro's turn this frame (degrees, its senses) to the active orbit camera's angles, the pitch
// within the camera's limits. False when no orbit camera is active.
bool turn(double pitch, double yaw);
std::string status();

}  // namespace look
}  // namespace gyro4all::glacier
