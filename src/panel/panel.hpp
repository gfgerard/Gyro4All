#pragma once
// gyro4all.dll internals shared by the frame side (panel_main.cpp) and the renderers (panel_dx11/dx10/dx9/vk/gl.cpp).
#include "gyro4all_panel.h"

#include <windows.h>
#include <unknwn.h>

#include <atomic>
#include <cstdint>

struct ImDrawData;

namespace g4a {

// One lock serializes everything that touches the ImGui context: the panel's frame on the game thread, window
// messages, and the renderers on the presenting thread (ImGui 1.92's renderer backends update the font textures of
// the context while they draw). Re-entrant (a Win32 critical section, not std::mutex: this DLL depends on as little of
// the C++ runtime as it can): Windows sends some messages synchronously from inside another one on the same thread
// (WM_CAPTURECHANGED from a capture change), and a non-recursive lock froze Mortal Shell's game thread there.
extern CRITICAL_SECTION g_lock;
struct Lock {
    Lock() { EnterCriticalSection(&g_lock); }
    ~Lock() { LeaveCriticalSection(&g_lock); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

void log(const char* fmt, ...);

// Under g_lock. The draw data of the last complete panel frame, or null (panel closed, or no frame since it opened).
ImDrawData* draw_data();

// A renderer tells the frame side what it draws on. Any thread.
void renderer_ready(bool ready);
void present_seen();
void backbuffer_size(uint32_t width, uint32_t height, bool framebuffer_scale);

// Renderers. Each is initialized on its first present, on the device it gets, and re-initialized when the game
// switches to another device. They return false when that API's panel can't work (logged once).
namespace dx11 {
bool present(IUnknown* swapchain, uint32_t color_space);
void before_resize();
}  // namespace dx11
namespace dx10 {
bool present(IUnknown* swapchain);
void before_resize();
}  // namespace dx10
namespace dx9 {
bool present(IUnknown* device);
void before_reset();
}  // namespace dx9
namespace vk {
bool present(g4a_vk_present& present);
void swapchain_destroyed(uint64_t swapchain);
void device_destroyed(void* device);
}  // namespace vk
namespace gl {
bool present(HDC hdc);
}  // namespace gl

}  // namespace g4a
