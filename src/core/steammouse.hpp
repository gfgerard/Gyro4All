#pragma once
// Steam Input's mouse (gyro to mouse, touchpad or stick as mouse) blocked or converted, in the games where GyroLib's
// own option (input.steam_mouse) doesn't act: its mouse bridge comes only with the DX12 panel, and even attached it
// confirms Steam's stream from Raw Input packets, which DirectInput games (DOOM 2016) never get (MODLOG 2026-10-31).
// Same route as GyroLib's: a low-level mouse hook on a thread of its own drops injected mouse motion (LLMHF_INJECTED)
// before Windows applies it to the cursor and legacy mouse messages. Raw Input still carries it (DirectInput 8 reads its
// mouse that way), so GetRawInputData/GetRawInputBuffer (inline, every caller) also empty device-less relative mouse
// packets while blocking. Physical mice, buttons and the wheel pass. Unlike GyroLib, any injected motion counts as
// Steam's while the controller is under Steam Input, without waiting for Raw Input to confirm it.

#include <cstdint>

struct SDL_Gamepad;
struct gl_context;

namespace gyro4all::steammouse {

struct Frame {
    gl_context* ctx = nullptr;
    SDL_Gamepad* pad = nullptr;  // GyroLib's selected controller (Steam's mouse is only taken while it's under Steam Input)
    uint32_t view = 0;           // the view GyroLib was given this frame (0: none; the last one's setting applies)
    bool focused = false;        // the game window is in front
    bool camera = false;         // the camera may turn (Convert's turn is dropped otherwise)
    bool gyrolib_panel = false;  // GyroLib's DX12 panel is attached: its own bridge applies the setting there
};

// Game thread, every frame, after gl_update. The setting is GyroLib's per-view input.steam_mouse (Pass through, Block,
// Convert movement). Block and Convert take Steam's mouse everywhere in the game (menus and the settings panel too)
// while it is focused; blocking lapses 150 ms after the last frame that allows it. pitch/yaw: Convert's turn
// this frame, in degrees (0 otherwise).
void frame(const Frame& f, double& pitch, double& yaw);

}  // namespace gyro4all::steammouse
