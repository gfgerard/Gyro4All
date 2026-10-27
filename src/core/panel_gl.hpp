#pragma once
// The settings panel in OpenGL games (panel_gl.cpp): import hooks on the game's buffer swaps that find its window and
// hand each swap to gyro4all.dll.

namespace gyro4all::panel_gl {

// DllMain, with the panel's other hooks (panel_host::install).
void install();

}  // namespace gyro4all::panel_gl
