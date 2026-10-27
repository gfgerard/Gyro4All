// The settings panel in OpenGL games: the game's buffer swaps (import hooks on gdi32's SwapBuffers, which DOOM 2016's
// OpenGL exe imports, and opengl32's wglSwapBuffers) name the window the game presents to, which becomes the panel's
// window (panel_host::set_gl_target), and each swap is handed to gyro4all.dll (g4a_api::gl_present) before the
// original: it draws the panel's last frame into the back buffer with the game's current GL context.
#include "panel_gl.hpp"

#include "hooks.hpp"
#include "log.hpp"
#include "panel_host.hpp"

#include <gyro4all_panel.h>

#include <atomic>

namespace gyro4all::panel_gl {
namespace {

using SwapBuffers_t = BOOL(WINAPI*)(HDC);

SwapBuffers_t g_swap_buffers = nullptr;      // gdi32.dll!SwapBuffers
SwapBuffers_t g_wgl_swap_buffers = nullptr;  // opengl32.dll!wglSwapBuffers
std::atomic<HWND> g_window{nullptr};
// Inside one of these hooks: a swap nested in it (SwapBuffers calling wglSwapBuffers through a hooked module) draws
// nothing, so the panel is drawn once per frame.
thread_local int t_swapping = 0;

template <class T>
T original(T hooked, const wchar_t* dll, const char* name) {
    if (hooked) return hooked;
    HMODULE m = GetModuleHandleW(dll);
    return m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
}

void before_swap(HDC hdc) {
    if (t_swapping || !hdc) return;
    const HWND window = WindowFromDC(hdc);
    if (!window) return;
    if (g_window.exchange(window) != window) panel_host::set_gl_target(window);
    if (const g4a_api* a = panel_host::api()) a->gl_present(hdc);
}

BOOL WINAPI hk_swap_buffers(HDC hdc) {
    before_swap(hdc);
    const auto real = original(g_swap_buffers, L"gdi32.dll", "SwapBuffers");
    ++t_swapping;
    const BOOL r = real ? real(hdc) : FALSE;
    --t_swapping;
    return r;
}

BOOL WINAPI hk_wgl_swap_buffers(HDC hdc) {
    before_swap(hdc);
    const auto real = original(g_wgl_swap_buffers, L"opengl32.dll", "wglSwapBuffers");
    ++t_swapping;
    const BOOL r = real ? real(hdc) : FALSE;
    --t_swapping;
    return r;
}

}  // namespace

void install() {
    hooks::add("gdi32.dll", "SwapBuffers", reinterpret_cast<void*>(&hk_swap_buffers),
               reinterpret_cast<void**>(&g_swap_buffers));
    hooks::add("opengl32.dll", "wglSwapBuffers", reinterpret_cast<void*>(&hk_wgl_swap_buffers),
               reinterpret_cast<void**>(&g_wgl_swap_buffers));
}

}  // namespace gyro4all::panel_gl
