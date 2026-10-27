// gyro4all.dll: GyroLib's static ImGui settings panel (the same widgets and settings as its DX12 frontend) drawn into the
// game's own frames when the game renders with DX11, DX10, DX9, Vulkan or OpenGL. This file is the frame side: the ImGui
// context, the Win32 platform backend on the game window, gamepad navigation from GyroLib's selected controller, input
// capture, and the 250 ms render heartbeat. The renderers live in panel_dx11.cpp, panel_dx10.cpp, panel_dx9.cpp,
// panel_vk.cpp and panel_gl.cpp.
//
// Threads: GyroLib's context has one owner thread (the game thread, where Gyro4All calls gl_update), and only GyroLib's
// DX12 frontend may draw from another one. So the panel's ImGui frame (NewFrame, gl_panel_draw, Render) is built on the
// game thread, and the presenting thread only renders the draw data of the last complete frame. g_lock keeps the two
// (and window messages) from ever running at the same time.
//
// Coexistence with other ImGui users in the game (ReShade, REFramework, UE4SS...): this DLL links its own ImGui and
// exports nothing of it, has its own context, writes no imgui.ini, never changes the mouse cursor and installs no input
// hooks of its own (winmm.dll forwards the game window's messages).
#include "gyro4all_panel.h"
#include "panel.hpp"

#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi.h>

#include <imgui.h>
#include <imgui_impl_win32.h>

#include <gyrolib/gyrolib.h>
#include <gyrolib/panel.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace g4a {

CRITICAL_SECTION g_lock;
namespace {
struct LockInit {
    LockInit() { InitializeCriticalSection(&g_lock); }
} g_lock_init;
}  // namespace

namespace {

g4a_log_fn g_log = nullptr;
gl_context* g_ctx = nullptr;  // owner thread only
gl_panel* g_panel = nullptr;
HWND g_hwnd = nullptr;

std::atomic<bool> g_open{false};    // the panel's ImGui frames are running (set and cleared on the owner thread)
std::atomic<bool> g_escape{false};  // Escape pressed while open: closes the panel unless a popup is open
std::atomic<uint32_t> g_capture{0};
std::atomic<bool> g_renderer{false};
std::atomic<ULONGLONG> g_last_present{0};
std::atomic<uint32_t> g_bb_width{0}, g_bb_height{0};
std::atomic<bool> g_bb_scale{false};  // the renderer honours ImDrawData::FramebufferScale (DX10, DX11)
bool g_has_frame = false;             // under g_lock: ImGui::GetDrawData() is the panel's last complete frame
ULONGLONG g_opened_at = 0;            // owner thread
int32_t g_last_result = 0;            // owner thread: gl_panel_last_result, logged on change

constexpr ULONGLONG kHeartbeatMs = 250;

// GyroLib's selected controller as ImGui gamepad keys, mapped like ImGui's SDL3 backend (the one GyroLib's own demo
// drives this panel with): by position, so Cross/A activates and Circle/B goes back on every layout.
void feed_gamepad(ImGuiIO& io, const g4a_gamepad* pad) {
    const bool connected = pad && pad->connected;
    if (connected) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    const uint32_t buttons = connected ? pad->buttons : 0;
    static const struct {
        ImGuiKey key;
        uint32_t bit;
    } kButtons[] = {
        {ImGuiKey_GamepadStart, G4A_PAD_START},          {ImGuiKey_GamepadBack, G4A_PAD_BACK},
        {ImGuiKey_GamepadFaceLeft, G4A_PAD_WEST},        {ImGuiKey_GamepadFaceRight, G4A_PAD_EAST},
        {ImGuiKey_GamepadFaceUp, G4A_PAD_NORTH},         {ImGuiKey_GamepadFaceDown, G4A_PAD_SOUTH},
        {ImGuiKey_GamepadDpadLeft, G4A_PAD_DPAD_LEFT},   {ImGuiKey_GamepadDpadRight, G4A_PAD_DPAD_RIGHT},
        {ImGuiKey_GamepadDpadUp, G4A_PAD_DPAD_UP},       {ImGuiKey_GamepadDpadDown, G4A_PAD_DPAD_DOWN},
        {ImGuiKey_GamepadL1, G4A_PAD_LEFT_SHOULDER},     {ImGuiKey_GamepadR1, G4A_PAD_RIGHT_SHOULDER},
        {ImGuiKey_GamepadL3, G4A_PAD_LEFT_STICK},        {ImGuiKey_GamepadR3, G4A_PAD_RIGHT_STICK},
    };
    for (const auto& b : kButtons) io.AddKeyEvent(b.key, (buttons & b.bit) != 0);
    // Analog keys: v0 = no press, v1 = full press (ImGui_ImplSDL3_UpdateGamepadAnalog, dead zone 8000).
    const auto analog = [&](ImGuiKey key, int axis, float v0, float v1) {
        const float raw = connected ? float(pad->axes[axis]) : v0;
        const float v = std::clamp((raw - v0) / (v1 - v0), 0.0f, 1.0f);
        io.AddKeyAnalogEvent(key, v > 0.1f, v);
    };
    constexpr float dz = 8000;
    analog(ImGuiKey_GamepadL2, G4A_AXIS_LEFT_TRIGGER, 0.0f, 32767);
    analog(ImGuiKey_GamepadR2, G4A_AXIS_RIGHT_TRIGGER, 0.0f, 32767);
    analog(ImGuiKey_GamepadLStickLeft, G4A_AXIS_LEFTX, -dz, -32768);
    analog(ImGuiKey_GamepadLStickRight, G4A_AXIS_LEFTX, +dz, +32767);
    analog(ImGuiKey_GamepadLStickUp, G4A_AXIS_LEFTY, -dz, -32768);
    analog(ImGuiKey_GamepadLStickDown, G4A_AXIS_LEFTY, +dz, +32767);
    analog(ImGuiKey_GamepadRStickLeft, G4A_AXIS_RIGHTX, -dz, -32768);
    analog(ImGuiKey_GamepadRStickRight, G4A_AXIS_RIGHTX, +dz, +32767);
    analog(ImGuiKey_GamepadRStickUp, G4A_AXIS_RIGHTY, -dz, -32768);
    analog(ImGuiKey_GamepadRStickDown, G4A_AXIS_RIGHTY, +dz, +32767);
}

// Mouse buttons polled every frame while the panel is open and the game window has the focus, besides the window
// messages (mouse_button): games that read the mouse through DirectInput or raw input without legacy messages get no
// WM_*BUTTON* messages (DOOM 2016's OpenGL exe: the panel's cursor moved, from GetCursorPos, and no click arrived).
// ImGui drops an event that repeats the state already queued, so a click seen both ways counts once. GetAsyncKeyState
// reads the physical buttons: swapped when the user swapped them in Windows.
void poll_mouse_buttons(ImGuiIO& io) {
    const HWND foreground = GetForegroundWindow();
    if (!foreground || GetAncestor(g_hwnd, GA_ROOT) != foreground) return;
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    const int keys[5] = {swapped ? VK_RBUTTON : VK_LBUTTON, swapped ? VK_LBUTTON : VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1,
                         VK_XBUTTON2};
    for (int button = 0; button < 5; ++button) {
        const bool down = (GetAsyncKeyState(keys[button]) & 0x8000) != 0;
        if (down != io.MouseDown[button]) {
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMouseButtonEvent(button, down);
        }
    }
}

// The panel's input state, logged 1 s and 3 s into each opening: once in DOOM: The Dark Ages the panel answered neither
// the mouse nor the d-pad in gameplay (cursor hidden), not reproduced since (MODLOG 2026-10-19).
void log_input_state(const ImGuiIO& io, const g4a_gamepad* pad, ULONGLONG since_open) {
    static int logged = 0;  // of this opening
    if (since_open < 1000) logged = 0;
    if (logged >= 2 || since_open < (logged ? 3000ull : 1000ull)) return;
    ++logged;
    POINT cursor{};
    GetCursorPos(&cursor);
    RECT clip{}, client{};
    GetClipCursor(&clip);
    GetClientRect(g_hwnd, &client);
    int mouse_down = 0;
    for (int i = 0; i < 5; ++i) mouse_down |= io.MouseDown[i] ? 1 << i : 0;
    log("input +%llums: imgui mouse (%.0f,%.0f) down %x, cursor (%ld,%ld) clip (%ld,%ld)-(%ld,%ld), client %ldx%ld, "
        "foreground %d focus %d appFocusLost %d, pad %d buttons %x, itemActive %d windowFocused %d hovered %d navActive %d "
        "navVisible %d wantMouse %d display %.0fx%.0f",
        static_cast<unsigned long long>(since_open), io.MousePos.x, io.MousePos.y, mouse_down, cursor.x, cursor.y,
        clip.left, clip.top, clip.right, clip.bottom, client.right, client.bottom,
        GetAncestor(g_hwnd, GA_ROOT) == GetForegroundWindow(), GetFocus() == g_hwnd, io.AppFocusLost ? 1 : 0,
        pad && pad->connected ? 1 : 0, pad ? pad->buttons : 0, ImGui::IsAnyItemActive(), ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow),
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow), io.NavActive, io.NavVisible, io.WantCaptureMouse,
        io.DisplaySize.x, io.DisplaySize.y);
}

// Owner thread: the panel stopped drawing (closed by GyroLib, by us, or never shown).
void stop_frames(const char* why) {
    if (!g_open.exchange(false)) return;
    {
        Lock lock;
        g_has_frame = false;
    }
    g_capture = 0;
    log("panel closed (%s)", why);
}

int32_t api_create(gl_context* ctx, void* hwnd, g4a_log_fn log_fn) {
    if (g_panel) return G4A_OK;
    g_log = log_fn;
    if (!ctx || !hwnd) return G4A_FAILED;
    Lock lock;
    if (!ImGui::DebugCheckVersionAndDataLayout(IMGUI_VERSION, sizeof(ImGuiIO), sizeof(ImGuiStyle), sizeof(ImVec2),
                                               sizeof(ImVec4), sizeof(ImDrawVert), sizeof(ImDrawIdx))) {
        log("ImGui %s headers don't match GyroLib's ImGui library; panel disabled", IMGUI_VERSION);
        return G4A_FAILED;
    }
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini in the game folder (nor reading another ImGui user's)
    io.LogFilename = nullptr;
    // As GyroLib's demo integrates this panel: its scalable default font, keyboard and gamepad navigation, and the
    // game owns the OS cursor.
    io.Fonts->AddFontDefaultVector();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad |
                      ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplWin32_Init(hwnd)) {
        log("ImGui's Win32 backend did not start on window %p; panel disabled", hwnd);
        ImGui::DestroyContext();
        return G4A_FAILED;
    }
    g_panel = gl_panel_create(ctx, nullptr);
    if (!g_panel) {
        log("gl_panel_create failed; panel disabled");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return G4A_FAILED;
    }
    g_ctx = ctx;
    g_hwnd = static_cast<HWND>(hwnd);
    log("GyroLib's static panel ready (ImGui %s) on window %p", IMGUI_VERSION, hwnd);
    return G4A_OK;
}

int32_t api_set_window(void* hwnd) {
    if (!g_panel || !hwnd) return G4A_UNAVAILABLE;
    if (hwnd == g_hwnd) return G4A_OK;
    Lock lock;
    ImGui_ImplWin32_Shutdown();
    if (!ImGui_ImplWin32_Init(hwnd)) {
        ImGui_ImplWin32_Init(g_hwnd);
        log("ImGui's Win32 backend did not start on window %p; the panel stays on window %p", hwnd,
            static_cast<void*>(g_hwnd));
        return G4A_FAILED;
    }
    log("the panel's input moves from window %p to window %p", static_cast<void*>(g_hwnd), hwnd);
    g_hwnd = static_cast<HWND>(hwnd);
    return G4A_OK;
}

int32_t api_function_key(uint32_t number, uint32_t pressed, uint32_t repeat) {
    if (!g_panel) return G4A_UNAVAILABLE;
    gl_panel_function_key(g_panel, number, pressed, repeat);
    return G4A_OK;
}

int32_t api_frame(const g4a_gamepad* pad) {
    if (!g_panel) return G4A_UNAVAILABLE;
    if (!gl_panel_open(g_ctx)) {
        g_opened_at = 0;
        stop_frames("by the panel");
        return G4A_OK;
    }
    const ULONGLONG now = GetTickCount64();
    if (!g_opened_at) g_opened_at = now;
    // Like GyroLib's DX12 frontend: no render for 250 ms closes the panel, so an invisible panel never keeps the gyro
    // and the controller suspended (renderer failed, game minimized or not presenting).
    const ULONGLONG last = std::max(g_last_present.load(), g_opened_at);
    if (!g_renderer || now - last > kHeartbeatMs) {
        if (now - last > kHeartbeatMs) {
            gl_set_panel_open(g_ctx, 0);
            g_opened_at = 0;
            log("no frame presented for %llu ms%s: panel closed", now - last, g_renderer ? "" : " (renderer not ready)");
            stop_frames("not presented");
        }
        return G4A_UNAVAILABLE;
    }

    Lock lock;
    ImGuiIO& io = ImGui::GetIO();
    if (!g_open) {
        // Fresh input on every opening: nothing typed or clicked while the panel was closed reaches it.
        io.ClearEventsQueue();
        io.ClearInputKeys();
        io.ClearInputMouse();
        g_escape = false;
        g_open = true;
        log("panel open");
    }
    if (g_escape.exchange(false) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        gl_set_panel_open(g_ctx, 0);
        g_opened_at = 0;
        g_has_frame = false;
        g_open = false;
        g_capture = 0;
        log("panel closed (Escape)");
        return G4A_OK;
    }
    ImGui_ImplWin32_NewFrame();
    // A backbuffer of another size than the window's client area (DPI virtualization, a fixed-resolution swapchain):
    // ImGui works in client coordinates (the mouse's) and the renderer scales to the backbuffer.
    const uint32_t bw = g_bb_width, bh = g_bb_height;
    io.DisplayFramebufferScale = ImVec2(1, 1);
    if (g_bb_scale && bw && bh && io.DisplaySize.x > 0 && io.DisplaySize.y > 0)
        io.DisplayFramebufferScale = ImVec2(float(bw) / io.DisplaySize.x, float(bh) / io.DisplaySize.y);
    feed_gamepad(io, pad);
    poll_mouse_buttons(io);
    // The game hides the OS cursor in gameplay (Mortal Shell: the panel answered the mouse with no cursor on screen):
    // then ImGui draws its own, and the game's cursor state is left alone.
    CURSORINFO cursor{sizeof(cursor)};
    const bool hidden = GetCursorInfo(&cursor) && (!(cursor.flags & CURSOR_SHOWING) || !cursor.hCursor);
    if (hidden != io.MouseDrawCursor) log("OS cursor %s: %s", hidden ? "hidden" : "shown", hidden ? "the panel draws its own" : "the panel uses it");
    io.MouseDrawCursor = hidden;
    ImGui::NewFrame();
    const UINT dpi = GetDpiForWindow(g_hwnd);
    gl_panel_draw(g_panel, io.DisplaySize.x, io.DisplaySize.y, dpi ? float(dpi) / 96.0f : 1.0f);
    ImGui::Render();
    log_input_state(io, pad, now - g_opened_at);
    g_has_frame = true;
    g_capture = G4A_CAPTURE_MOUSE | G4A_CAPTURE_KEYBOARD | G4A_CAPTURE_GAMEPAD;
    if (const int32_t r = gl_panel_last_result(g_panel); r != g_last_result) {
        g_last_result = r;
        log("gl_panel_draw result %d", r);
    }
    return G4A_OK;
}

// Mouse buttons go to ImGui here, not through its Win32 backend: the backend calls SetCapture/ReleaseCapture around
// clicks, which would take the game's own mouse capture away (Unreal holds it) and sends WM_CAPTURECHANGED back into
// this window procedure synchronously.
bool mouse_button(ImGuiIO& io, uint32_t message, uint64_t wparam) {
    int button = -1;
    bool down = true;
    switch (message) {
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: button = 0; break;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: button = 1; break;
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: button = 2; break;
        case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: button = GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 3 : 4; break;
        case WM_LBUTTONUP: button = 0, down = false; break;
        case WM_RBUTTONUP: button = 1, down = false; break;
        case WM_MBUTTONUP: button = 2, down = false; break;
        case WM_XBUTTONUP: button = GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 3 : 4, down = false; break;
        default: return false;
    }
    io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
    io.AddMouseButtonEvent(button, down);
    return true;
}

uint32_t api_window_message(void* hwnd, uint32_t message, uint64_t wparam, int64_t lparam) {
    if (!g_open) return 0;
    {
        Lock lock;
        if (g_open && !mouse_button(ImGui::GetIO(), message, wparam))
            ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd), message, WPARAM(wparam), LPARAM(lparam));
    }
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE && !(lparam & (1ll << 30))) g_escape = true;
    const bool mouse = (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_INPUT;
    const bool keyboard = (message >= WM_KEYFIRST && message <= WM_KEYLAST) || message == WM_INPUT;
    const uint32_t cap = g_capture;
    return (mouse ? cap & G4A_CAPTURE_MOUSE : 0) | (keyboard ? cap & G4A_CAPTURE_KEYBOARD : 0);
}

uint32_t api_capture() { return g_capture; }

uint32_t api_ready() { return g_renderer && GetTickCount64() - g_last_present.load() <= kHeartbeatMs ? 1u : 0u; }

// DXGI swapchains: D3D11 or D3D10, decided once per swapchain from the device it was made on.
int32_t api_dxgi_present(void* swapchain, uint32_t color_space) {
    if (!g_panel || !swapchain) return G4A_UNAVAILABLE;
    static void* last = nullptr;
    static int api = 0;  // 11, 10, or -1 = neither
    IUnknown* sc = static_cast<IUnknown*>(swapchain);
    if (swapchain != last) {
        last = swapchain;
        IDXGISwapChain* dxgi = nullptr;
        IUnknown* device = nullptr;
        api = -1;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&dxgi)))) {
            if (SUCCEEDED(dxgi->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) api = 11;
            else if (SUCCEEDED(dxgi->GetDevice(__uuidof(ID3D10Device), reinterpret_cast<void**>(&device)))) api = 10;
            if (device) device->Release();
            dxgi->Release();
        }
        log("swapchain %p is %s", swapchain, api == 11 ? "D3D11" : api == 10 ? "D3D10" : "neither D3D11 nor D3D10: not drawn");
    }
    const bool ok = api == 11 ? dx11::present(sc, color_space) : api == 10 ? dx10::present(sc) : false;
    return ok ? G4A_OK : G4A_UNAVAILABLE;
}

void api_dxgi_before_resize(void*) {
    dx11::before_resize();
    dx10::before_resize();
}

int32_t api_dx9_present(void* device) {
    if (!g_panel || !device) return G4A_UNAVAILABLE;
    return dx9::present(static_cast<IUnknown*>(device)) ? G4A_OK : G4A_UNAVAILABLE;
}

void api_dx9_before_reset(void*) { dx9::before_reset(); }

int32_t api_vk_present(g4a_vk_present* present) {
    if (!g_panel || !present || present->size < sizeof(g4a_vk_present)) return G4A_UNAVAILABLE;
    return vk::present(*present) ? G4A_OK : G4A_UNAVAILABLE;
}

void api_vk_swapchain_destroyed(uint64_t swapchain) { vk::swapchain_destroyed(swapchain); }

void api_vk_device_destroyed(void* device) { vk::device_destroyed(device); }

int32_t api_gl_present(void* hdc) {
    if (!g_panel || !hdc) return G4A_UNAVAILABLE;
    return gl::present(static_cast<HDC>(hdc)) ? G4A_OK : G4A_UNAVAILABLE;
}

const g4a_api kApi = {
    sizeof(g4a_api),      G4A_ABI_VERSION,        &api_create,      &api_function_key,     &api_frame,
    &api_window_message,  &api_capture,           &api_ready,       &api_dxgi_present,     &api_dxgi_before_resize,
    &api_dx9_present,     &api_dx9_before_reset,  &api_vk_present,  &api_vk_swapchain_destroyed,
    &api_vk_device_destroyed, &api_gl_present,   &api_set_window,
};

}  // namespace

void log(const char* fmt, ...) {
    if (!g_log) return;
    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    g_log(text);
}

ImDrawData* draw_data() {
    if (!g_has_frame || !g_open) return nullptr;
    ImDrawData* data = ImGui::GetDrawData();
    return data && data->Valid ? data : nullptr;
}

void renderer_ready(bool ready) { g_renderer = ready; }

void present_seen() { g_last_present = GetTickCount64(); }

void backbuffer_size(uint32_t width, uint32_t height, bool framebuffer_scale) {
    g_bb_width = width;
    g_bb_height = height;
    g_bb_scale = framebuffer_scale;
}

}  // namespace g4a

extern "C" const g4a_api* g4a_get_api(uint32_t abi_version) {
    return abi_version == G4A_ABI_VERSION ? &g4a::kApi : nullptr;
}
