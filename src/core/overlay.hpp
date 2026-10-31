#pragma once
// Optional in-game settings panel, enabled only when gyrolib.ini has a menu key or the gamepad shortcut (otherwise no
// graphics hooks are installed at all). DX12 games: GyroLib's DX12 overlay inside gyrolib.dll. D3D11, D3D10 and D3D9
// games: GyroLib's static ImGui panel (the same widgets and settings), drawn by gyro4all.dll (panel_host.hpp).
#include <string>

struct gl_context;
struct SDL_Gamepad;

namespace gyro4all::overlay {

// DllMain (before the renderer starts): decide from gyrolib.ini and hook DXGI factory creation.
void install(const std::wstring& settings_path);

// Game thread, after GyroLib's context exists. Returns true while a renderer is available.
bool attach(gl_context* ctx);
// Game thread, before every gl_update.
void process();
// True when the panel renderer is usable (DX12 swapchain captured and overlay created, or gyro4all.dll drawing).
bool available();
// Game thread, after gl_update: the static panel's frame (D3D11/D3D10/D3D9 games), navigated with GyroLib's selected
// controller. Nothing for DX12 games.
void draw(SDL_Gamepad* pad);
// Mask of GL_OVERLAY_CAPTURE_* bits while the panel is open.
unsigned capture();
// True once GyroLib's DX12 panel is attached (it brings GyroLib's own Steam Input mouse option).
bool gyrolib_panel();
// Game thread: poll the configured F-key (gl_get_menu_key) and toggle the panel on its press.
// Window messages for that key are no longer forwarded to GyroLib, so it never toggles twice.
void poll_menu_key(gl_context* ctx, bool focused);

// Any thread: fn is called at every Present of the game's swapchain (not test or nested ones), before the panel is drawn,
// for engines that time Gyro4All's frame on the game's frames (Frostbite: Engine::frame_source). Needs the panel's
// graphics hooks, which are only installed when gyrolib.ini enables the panel. Keep fn short.
using PresentFn = void (*)();
void on_present(PresentFn fn);

}  // namespace gyro4all::overlay
