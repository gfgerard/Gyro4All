#include "panel_host.hpp"

#include "com.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "panel_gl.hpp"
#include "panel_vk.hpp"
#include "settings.hpp"

#include <gyro4all_panel.h>

#include <d3d9.h>

#include <SDL3/SDL.h>

#include <atomic>
#include <cstring>
#include <string>

namespace gyro4all::panel_host {
namespace {

std::atomic<void*> g_target{nullptr};  // IDXGISwapChain* (D3D10/D3D11) or IDirect3DDevice9*
std::atomic<HWND> g_window{nullptr};
std::atomic<ULONGLONG> g_target_presented{0};  // GetTickCount64 of g_target's last Present (or its capture)
std::atomic<uint64_t> g_vk_target{0};  // VkSwapchainKHR (Vulkan games, panel_vk.cpp)
std::atomic<bool> g_gl_target{false};  // the game swaps OpenGL buffers (panel_gl.cpp)
std::atomic<uint32_t> g_color_space{G4A_COLOR_SPACE_UNKNOWN};
std::atomic<const g4a_api*> g_api{nullptr};
bool g_load_failed = false;  // game thread
HWND g_panel_window = nullptr;  // game thread: the window the panel's input is on
// Inside one of our D3D9 present hooks: a Present nested in it (the device's Present calling its swapchain's) draws
// nothing, so the panel is drawn once per frame.
thread_local int t_presenting = 0;

void log_line(const char* line) { log("panel: %s", line); }

// ---------------------------------------------------------------- D3D9
//
// Direct3DCreate9(Ex) (import hooks) -> IDirect3D9::CreateDevice (16) / IDirect3D9Ex::CreateDeviceEx (20) -> the device's
// Reset (16), Present (17), PresentEx (121), ResetEx (132), and its implicit swapchain's Present (3).
// Wrappers (ReShade as d3d9.dll) hand the game a proxy device whose Present runs their effects and overlay, then the real
// device's. The panel belongs on the real one, after them: the real d3d9.dll's IDirect3D9 vtable is hooked too, and a
// device created inside another hooked CreateDevice (a wrapper creating the real device) wins over the proxy around it.

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
using Direct3DCreate9Ex_t = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                   IDirect3DDevice9**);
using CreateDeviceEx_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                     D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using PresentEx_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using ResetEx_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
using SwapChainPresent_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND,
                                                       const RGNDATA*, DWORD);

Direct3DCreate9_t g_create9 = nullptr;
Direct3DCreate9Ex_t g_create9ex = nullptr;
CreateDevice_t g_create_device = nullptr;
CreateDeviceEx_t g_create_device_ex = nullptr;
Reset_t g_reset = nullptr;
Present_t g_present = nullptr;
PresentEx_t g_present_ex = nullptr;
ResetEx_t g_reset_ex = nullptr;
SwapChainPresent_t g_swapchain_present = nullptr;
std::atomic<bool> g_native_hooked{false};
bool g_native_allowed = true;  // OverlayFactory=game: only the objects the game itself gets (diagnostic)
thread_local int t_create_depth = 0;
thread_local bool t_inner_device = false;

// The real export when the import hook had no original yet (a delay-load resolved after the hook was registered).
template <class T>
T resolve(T& original, const char* function) {
    if (!original) {
        std::string dll = hooks::source_dll(function);
        HMODULE m = GetModuleHandleA(dll.empty() ? "d3d9.dll" : dll.c_str());
        if (m) original = reinterpret_cast<T>(GetProcAddress(m, function));
    }
    return original;
}

void draw(IDirect3DDevice9* device) {
    const g4a_api* a = g_api.load();
    if (!a || t_presenting || static_cast<void*>(device) != g_target.load()) return;
    a->dx9_present(device);
}

HRESULT STDMETHODCALLTYPE hk_present(IDirect3DDevice9* d, const RECT* src, const RECT* dst, HWND w, const RGNDATA* dirty) {
    draw(d);
    ++t_presenting;
    const HRESULT hr = com::original_of(d, 17, g_present)(d, src, dst, w, dirty);
    --t_presenting;
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_present_ex(IDirect3DDevice9Ex* d, const RECT* src, const RECT* dst, HWND w,
                                        const RGNDATA* dirty, DWORD flags) {
    draw(d);
    ++t_presenting;
    const HRESULT hr = com::original_of(d, 121, g_present_ex)(d, src, dst, w, dirty, flags);
    --t_presenting;
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_swapchain_present(IDirect3DSwapChain9* sc, const RECT* src, const RECT* dst, HWND w,
                                               const RGNDATA* dirty, DWORD flags) {
    if (!t_presenting) {
        IDirect3DDevice9* device = nullptr;
        if (SUCCEEDED(sc->GetDevice(&device))) {
            draw(device);
            device->Release();
        }
    }
    ++t_presenting;
    const HRESULT hr = com::original_of(sc, 3, g_swapchain_present)(sc, src, dst, w, dirty, flags);
    --t_presenting;
    return hr;
}

void before_reset(IDirect3DDevice9* d) {
    const g4a_api* a = g_api.load();
    if (a && static_cast<void*>(d) == g_target.load()) a->dx9_before_reset(d);
}

HRESULT STDMETHODCALLTYPE hk_reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
    before_reset(d);
    const HRESULT hr = com::original_of(d, 16, g_reset)(d, pp);
    log("panel: D3D9 Reset -> 0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_reset_ex(IDirect3DDevice9Ex* d, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode) {
    before_reset(d);
    const HRESULT hr = com::original_of(d, 132, g_reset_ex)(d, pp, mode);
    log("panel: D3D9 ResetEx -> 0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}

void device_created(IDirect3DDevice9* device, HWND window, const char* via) {
    const bool nested = t_create_depth > 0;
    if (!nested && t_inner_device) {
        if (static_cast<void*>(device) != g_target.load())
            log("panel: D3D9 device %p from %s not used: the panel draws on the device it wraps", static_cast<void*>(device),
                via);
        return;
    }
    if (nested) t_inner_device = true;
    void** vtable = com::vtable_of(device);
    com::patch_slot(vtable, 16, reinterpret_cast<void*>(&hk_reset), reinterpret_cast<void**>(&g_reset));
    com::patch_slot(vtable, 17, reinterpret_cast<void*>(&hk_present), reinterpret_cast<void**>(&g_present));
    IDirect3DDevice9Ex* ex = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&ex)))) {
        void** ex_vtable = com::vtable_of(ex);
        com::patch_slot(ex_vtable, 121, reinterpret_cast<void*>(&hk_present_ex), reinterpret_cast<void**>(&g_present_ex));
        com::patch_slot(ex_vtable, 132, reinterpret_cast<void*>(&hk_reset_ex), reinterpret_cast<void**>(&g_reset_ex));
        ex->Release();
    }
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &sc))) {
        com::patch_slot(com::vtable_of(sc), 3, reinterpret_cast<void*>(&hk_swapchain_present),
                        reinterpret_cast<void**>(&g_swapchain_present));
        sc->Release();
    }
    g_target = device;
    g_window = window;
    log("panel: captured D3D9%s device %p (%s) for window %p", ex ? "Ex" : "", static_cast<void*>(device), via,
        static_cast<void*>(window));
}

HRESULT STDMETHODCALLTYPE hk_create_device(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                           D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    if (t_create_depth == 0) t_inner_device = false;
    ++t_create_depth;
    const HRESULT hr = com::original_of(d3d, 16, g_create_device)(d3d, adapter, type, focus, flags, pp, out);
    --t_create_depth;
    if (SUCCEEDED(hr) && out && *out) device_created(*out, pp && pp->hDeviceWindow ? pp->hDeviceWindow : focus, "CreateDevice");
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_create_device_ex(IDirect3D9Ex* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                              D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** out) {
    if (t_create_depth == 0) t_inner_device = false;
    ++t_create_depth;
    const HRESULT hr = com::original_of(d3d, 20, g_create_device_ex)(d3d, adapter, type, focus, flags, pp, mode, out);
    --t_create_depth;
    if (SUCCEEDED(hr) && out && *out)
        device_created(*out, pp && pp->hDeviceWindow ? pp->hDeviceWindow : focus, "CreateDeviceEx");
    return hr;
}

void hook_d3d9_object(IDirect3D9* d3d, bool ex) {
    void** vtable = com::vtable_of(d3d);
    com::patch_slot(vtable, 16, reinterpret_cast<void*>(&hk_create_device), reinterpret_cast<void**>(&g_create_device));
    if (ex)
        com::patch_slot(vtable, 20, reinterpret_cast<void*>(&hk_create_device_ex),
                        reinterpret_cast<void**>(&g_create_device_ex));
}

// The real System32 d3d9.dll's object vtables, under any wrapper DLL of the same name.
void hook_native_d3d9() {
    if (!g_native_allowed || g_native_hooked.exchange(true)) return;
    wchar_t path[MAX_PATH];
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    lstrcpynW(path + n, L"\\d3d9.dll", MAX_PATH - int(n));
    HMODULE d3d9 = LoadLibraryW(path);
    auto create = d3d9 ? reinterpret_cast<Direct3DCreate9_t>(GetProcAddress(d3d9, "Direct3DCreate9")) : nullptr;
    auto create_ex = d3d9 ? reinterpret_cast<Direct3DCreate9Ex_t>(GetProcAddress(d3d9, "Direct3DCreate9Ex")) : nullptr;
    bool ok = false;
    if (IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr) {
        hook_d3d9_object(d3d, false);
        d3d->Release();
        ok = true;
    }
    IDirect3D9Ex* d3d_ex = nullptr;
    if (create_ex && SUCCEEDED(create_ex(D3D_SDK_VERSION, &d3d_ex)) && d3d_ex) {
        hook_d3d9_object(d3d_ex, true);
        d3d_ex->Release();
    }
    log("panel: System32 d3d9.dll's device creation %s", ok ? "hooked" : "not hooked");
}

IDirect3D9* WINAPI hk_direct3d_create9(UINT sdk) {
    hook_native_d3d9();
    IDirect3D9* d3d = resolve(g_create9, "Direct3DCreate9") ? g_create9(sdk) : nullptr;
    if (d3d) hook_d3d9_object(d3d, false);
    return d3d;
}

HRESULT WINAPI hk_direct3d_create9ex(UINT sdk, IDirect3D9Ex** out) {
    hook_native_d3d9();
    const HRESULT hr = resolve(g_create9ex, "Direct3DCreate9Ex") ? g_create9ex(sdk, out) : E_NOINTERFACE;
    if (SUCCEEDED(hr) && out && *out) hook_d3d9_object(*out, true);
    return hr;
}

// ---------------------------------------------------------------- the DLL

const g4a_api* load(gl_context* ctx, HWND window) {
    const std::wstring path = settings::install_dir() + L"\\gyro4all.dll";
    HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) {
        log("panel: can't load %s (error %lu); no settings panel in this game", narrow(path).c_str(), GetLastError());
        return nullptr;
    }
    auto get = reinterpret_cast<g4a_get_api_fn>(GetProcAddress(dll, "g4a_get_api"));
    const g4a_api* a = get ? get(G4A_ABI_VERSION) : nullptr;
    if (!a || a->size < sizeof(g4a_api)) {
        log("panel: gyro4all.dll doesn't match this winmm.dll (interface %u); no settings panel", G4A_ABI_VERSION);
        return nullptr;
    }
    if (a->create(ctx, window, &log_line) != G4A_OK) return nullptr;
    log("panel: gyro4all.dll loaded");
    return a;
}

}  // namespace

void install() {
    if (hooks::inline_mode()) {
        log("panel: protected exe: no D3D9, Vulkan or OpenGL hooks (the D3D10/D3D11 panel still comes through DXGI)");
        return;
    }
    panel_vk::install();
    panel_gl::install();
    g_native_allowed = _stricmp(settings::get_string(L"OverlayFactory").c_str(), "game") != 0;
    hooks::add("d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&hk_direct3d_create9),
               reinterpret_cast<void**>(&g_create9));
    hooks::add("d3d9.dll", "Direct3DCreate9Ex", reinterpret_cast<void*>(&hk_direct3d_create9ex),
               reinterpret_cast<void**>(&g_create9ex));
}

void* runtime_export(const char* function, void* real) {
    if (hooks::inline_mode() || !real) return nullptr;
    if (std::strcmp(function, "Direct3DCreate9") == 0) {
        if (!g_create9) g_create9 = reinterpret_cast<Direct3DCreate9_t>(real);
        return reinterpret_cast<void*>(&hk_direct3d_create9);
    }
    if (std::strcmp(function, "Direct3DCreate9Ex") == 0) {
        if (!g_create9ex) g_create9ex = reinterpret_cast<Direct3DCreate9Ex_t>(real);
        return reinterpret_cast<void*>(&hk_direct3d_create9ex);
    }
    return nullptr;
}

void set_dxgi_target(void* swapchain, HWND window, const char* api) {
    if (g_target.exchange(swapchain) != swapchain) g_color_space = G4A_COLOR_SPACE_UNKNOWN;
    g_target_presented = GetTickCount64();
    const HWND previous = g_window.exchange(window);
    log("panel: captured %s swapchain %p for window %p", api, swapchain, static_cast<void*>(window));
    if (g_api.load() && previous && previous != window)
        log("panel: the game presents to another window now; the panel's input follows it");
}

bool is_dxgi_target(const void* swapchain) { return swapchain && swapchain == g_target.load(); }

void* target() { return g_target.load(); }

ULONGLONG target_idle_ms() { return GetTickCount64() - g_target_presented.load(); }

void set_vk_target(uint64_t swapchain, HWND window) {
    g_vk_target = swapchain;
    const HWND previous = g_window.exchange(window);
    log("panel: captured Vulkan swapchain %llx for window %p", static_cast<unsigned long long>(swapchain),
        static_cast<void*>(window));
    if (g_api.load() && previous && previous != window)
        log("panel: the game presents to another window now; the panel's input follows it");
}

void set_gl_target(HWND window) {
    g_gl_target = true;
    const HWND previous = g_window.exchange(window);
    log("panel: the game swaps OpenGL buffers on window %p", static_cast<void*>(window));
    if (g_api.load() && previous && previous != window)
        log("panel: the game presents to another window now; the panel's input follows it");
}

void set_color_space(const void* swapchain, uint32_t color_space) {
    if (!is_dxgi_target(swapchain)) return;
    if (g_color_space.exchange(color_space) != color_space)
        log("panel: the game sets colour space %u on its swapchain", color_space);
}

bool attach(gl_context* ctx) {
    if (g_api.load()) return true;
    if (g_load_failed || (!g_target.load() && !g_vk_target.load() && !g_gl_target.load()) || !g_window.load()) return false;
    const HWND window = g_window.load();
    const g4a_api* a = load(ctx, window);
    if (!a) {
        g_load_failed = true;
        return false;
    }
    g_panel_window = window;
    g_api = a;
    return true;
}

HWND follow_window() {
    const g4a_api* a = g_api.load();
    const HWND window = g_window.load();
    if (!a || !window || window == g_panel_window || !IsWindow(window)) return nullptr;
    if (a->set_window(window) != G4A_OK) {
        g_panel_window = window;  // tried once: the panel stays where it was
        return nullptr;
    }
    g_panel_window = window;
    return window;
}

const g4a_api* api() { return g_api.load(); }

HWND window() { return g_window.load(); }

void frame(SDL_Gamepad* pad) {
    const g4a_api* a = g_api.load();
    if (!a) return;
    g4a_gamepad gp{};
    if (pad) {
        gp.connected = 1;
        static const struct {
            SDL_GamepadButton button;
            uint32_t bit;
        } kButtons[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, G4A_PAD_SOUTH},
            {SDL_GAMEPAD_BUTTON_EAST, G4A_PAD_EAST},
            {SDL_GAMEPAD_BUTTON_WEST, G4A_PAD_WEST},
            {SDL_GAMEPAD_BUTTON_NORTH, G4A_PAD_NORTH},
            {SDL_GAMEPAD_BUTTON_BACK, G4A_PAD_BACK},
            {SDL_GAMEPAD_BUTTON_START, G4A_PAD_START},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, G4A_PAD_LEFT_STICK},
            {SDL_GAMEPAD_BUTTON_RIGHT_STICK, G4A_PAD_RIGHT_STICK},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, G4A_PAD_LEFT_SHOULDER},
            {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, G4A_PAD_RIGHT_SHOULDER},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, G4A_PAD_DPAD_UP},
            {SDL_GAMEPAD_BUTTON_DPAD_DOWN, G4A_PAD_DPAD_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, G4A_PAD_DPAD_LEFT},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, G4A_PAD_DPAD_RIGHT},
        };
        for (const auto& b : kButtons)
            if (SDL_GetGamepadButton(pad, b.button)) gp.buttons |= b.bit;
        static const SDL_GamepadAxis kAxes[G4A_AXIS_COUNT] = {
            SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
            SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
        };
        for (int i = 0; i < G4A_AXIS_COUNT; ++i) gp.axes[i] = SDL_GetGamepadAxis(pad, kAxes[i]);
    }
    a->frame(&gp);
}

void dxgi_present(void* swapchain) {
    const g4a_api* a = g_api.load();
    if (!is_dxgi_target(swapchain)) return;
    g_target_presented = GetTickCount64();
    if (a) a->dxgi_present(swapchain, g_color_space.load());
}

void dxgi_before_resize(void* swapchain) {
    const g4a_api* a = g_api.load();
    if (a && is_dxgi_target(swapchain)) a->dxgi_before_resize(swapchain);
}

}  // namespace gyro4all::panel_host
