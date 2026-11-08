#pragma once
// The settings panel for games that don't render with DX12: GyroLib's static ImGui panel, in Gyro4All\gyro4all.dll
// (src/panel/gyro4all_panel.h), drawn into the game's D3D11/D3D10 swapchain (found by overlay.cpp's DXGI hooks), its
// D3D9 device (hooked here), its Vulkan swapchain (panel_vk.cpp) or its OpenGL back buffer (panel_gl.cpp). The DLL is loaded only once the game has made such a swapchain or device, and only when the
// panel is enabled in gyrolib.ini; DX12 games never load it (GyroLib's DX12 frontend draws their panel).
#include <windows.h>

#include <cstdint>

struct gl_context;
struct g4a_api;
struct SDL_Gamepad;

namespace gyro4all::panel_host {

// DllMain, when the panel is enabled: the D3D9, Vulkan and OpenGL hooks (D3D10/D3D11 come through overlay.cpp).
void install();
// overlay.cpp's GetProcAddress hook: a game module looked up d3d9.dll's `function` at run time (real = the export).
// Returns the hook to hand out instead (keeping real as its original), or nullptr when it isn't one of ours.
void* runtime_export(const char* function, void* real);

// overlay.cpp: the D3D10/D3D11 swapchain the panel draws on (the innermost one when wrappers are stacked), and its
// window. Never AddRef'd: compared, and handed to gyro4all.dll only inside the game's own calls on it.
void set_dxgi_target(void* swapchain, HWND window, const char* api);
bool is_dxgi_target(const void* swapchain);
// The D3D10/D3D11 target (only compared or logged), and the time since it last presented or was captured.
void* target();
ULONGLONG target_idle_ms();
// The D3D10/D3D11 target presented at least once since it was captured.
bool target_has_presented();
// panel_vk.cpp: the Vulkan swapchain the panel draws on (VkSwapchainKHR) and its window.
void set_vk_target(uint64_t swapchain, HWND window);
// panel_gl.cpp: the window the game swaps OpenGL buffers on (the panel draws into its back buffer).
void set_gl_target(HWND window);
void set_color_space(const void* swapchain, uint32_t color_space);

// Game thread, every frame: loads gyro4all.dll and creates the panel once a target exists. False until then (or when
// the DLL can't be used). window(): the game window the panel works on.
bool attach(gl_context* ctx);
// overlay.cpp, DX12 games: attach the panel DLL for the Gyro4All window (no swapchain target of its own; window is
// the captured DX12 swapchain's). The panel draws through dx12_present, after GyroLib's DX12 overlay.
bool attach_dx12(gl_context* ctx, HWND window);
// Game thread, after attach: the window the game presents to now when it isn't the panel's any more (the panel's input
// moved there), else nullptr. dx12_window: the captured DX12 swapchain's window when the panel is in its DX12 mode (a
// D3D10/D3D11 swapchain the game also makes doesn't move it), else nullptr.
HWND follow_window(HWND dx12_window);
const g4a_api* api();
HWND window();

// Game thread, after gl_update: the panel's frame, navigated with GyroLib's selected controller.
void frame(SDL_Gamepad* pad);

// overlay.cpp's DXGI hooks, on the game's presenting/resizing threads.
void dxgi_present(void* swapchain);
void dxgi_before_resize(void* swapchain);
// overlay.cpp's DX12 swapchain hooks, on the render worker (inside the game's Present/ResizeBuffers calls, after
// GyroLib's overlay rendered on the same queue).
void dx12_present(void* swapchain, void* command_queue, uint32_t color_space);
void dx12_before_resize(void* swapchain);
void dx12_lost();
// host.cpp's L3+R3 chord while the panel is open: fold/unfold the Gyro4All settings window.
void expand_toggle();
// The Gyro4All window's last expand/fold/close (G4A_SETTINGS_*), whether it is expanded, and folding it without
// reopening GyroLib's panel (overlay.cpp keeps GyroLib's panel closed while the window is expanded).
uint32_t settings_event();
bool settings_expanded();
void close_settings();

}  // namespace gyro4all::panel_host
