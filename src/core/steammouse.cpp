#include "steammouse.hpp"

#include "dinputpad.hpp"
#include "hooks.hpp"
#include "log.hpp"

#include <windows.h>

#include <SDL3/SDL.h>
#include <gyrolib/gyrolib.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace gyro4all::steammouse {

namespace {

// Read by the hook thread, never locked: the game thread renews the lease every frame, so a stalled game (or a lost
// focus the frame hasn't seen yet) stops the blocking within kLeaseMs.
constexpr ULONGLONG kLeaseMs = 150;
std::atomic<ULONGLONG> g_block_until{0};
std::atomic<unsigned> g_blocked{0};
const DWORD g_pid = GetCurrentProcessId();

LRESULT CALLBACK hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_MOUSEMOVE &&
        (reinterpret_cast<const MSLLHOOKSTRUCT*>(lp)->flags & LLMHF_INJECTED) &&
        GetTickCount64() <= g_block_until.load(std::memory_order_relaxed)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == g_pid) {
            g_blocked.fetch_add(1, std::memory_order_relaxed);
            return 1;  // dropped before Windows applies it
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// The low-level hook keeps Steam's motion from the cursor and legacy mouse messages, but Windows still delivers it as
// Raw Input (GyroLib's docs say so too), and DirectInput 8 reads its mouse through Raw Input (dinput8.dll resolves
// RegisterRawInputDevices and GetRawInputData at run time): DOOM 2016 still turned with it. So every caller's
// GetRawInputData and GetRawInputBuffer (inline hooks: dinput8's own reads included) also get device-less relative
// mouse packets, the injected ones, with no motion while blocking (buttons and wheel kept).
std::atomic<unsigned> g_zeroed{0};

bool blocking_now() {
    if (GetTickCount64() > g_block_until.load(std::memory_order_relaxed)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == g_pid;
}

// Convert: the emptied packets' counts, taken by the game thread each frame (GyroLib's mouse_route.hpp does the same
// with the packets its bridge sees).
std::atomic<bool> g_convert{false};
std::atomic<long long> g_count_x{0}, g_count_y{0};

void zero_injected(RAWINPUT* raw, bool count = true) {
    if (raw->header.dwType != RIM_TYPEMOUSE || raw->header.hDevice || (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) ||
        (!raw->data.mouse.lLastX && !raw->data.mouse.lLastY))
        return;
    if (count && g_convert.load(std::memory_order_relaxed)) {
        g_count_x.fetch_add(raw->data.mouse.lLastX, std::memory_order_relaxed);
        g_count_y.fetch_add(raw->data.mouse.lLastY, std::memory_order_relaxed);
    }
    raw->data.mouse.lLastX = raw->data.mouse.lLastY = 0;
    g_zeroed.fetch_add(1, std::memory_order_relaxed);
}

using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBuffer_t = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
GetRawInputData_t g_get_data = nullptr;
GetRawInputBuffer_t g_get_buffer = nullptr;

// The same packet may be read more than once (Gyro4All's own PeekMessage hook reads each WM_INPUT before the game
// does): Convert counts each handle once.
std::atomic<HRAWINPUT> g_counted{nullptr};

UINT WINAPI hk_get_data(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header_size) {
    const UINT n = g_get_data(input, command, data, size, header_size);
    if (command == RID_INPUT && data && n != UINT(-1) && n >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE) && blocking_now()) {
        zero_injected(static_cast<RAWINPUT*>(data), g_counted.exchange(input) != input);
    }
    return n;
}

UINT WINAPI hk_get_buffer(PRAWINPUT data, PUINT size, UINT header_size) {
    const UINT n = g_get_buffer(data, size, header_size);
    if (data && n != UINT(-1) && n && blocking_now()) {
        PRAWINPUT block = data;
        for (UINT i = 0; i < n; ++i) {  // NEXTRAWINPUTBLOCK: each block starts 8-byte aligned after the previous one
            zero_injected(block);
            block = reinterpret_cast<PRAWINPUT>((reinterpret_cast<uintptr_t>(block) + block->header.dwSize + 7) & ~uintptr_t(7));
        }
    }
    return n;
}

// win32u's NtUserGetRawInputData/NtUserGetRawInputBuffer, which user32's functions only wrap: every caller still goes
// through them, and they stay free when user32's exports are already hooked inline (protected exes: xinput.cpp and
// nativepad.cpp hook user32's GetRawInputData there, and MinHook hooks a function once; Mass Effect 3 LE). user32's
// exports when win32u lacks them.
void* raw_function(const char* nt_name, const char* name, const char*& from) {
    if (HMODULE win32u = GetModuleHandleW(L"win32u.dll"))
        if (void* f = reinterpret_cast<void*>(GetProcAddress(win32u, nt_name))) {
            from = "win32u";
            return f;
        }
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    from = "user32";
    return user32 ? reinterpret_cast<void*>(GetProcAddress(user32, name)) : nullptr;
}

void hook_raw_input() {
    const char *data_from = "", *buffer_from = "";
    void* data = raw_function("NtUserGetRawInputData", "GetRawInputData", data_from);
    void* buffer = raw_function("NtUserGetRawInputBuffer", "GetRawInputBuffer", buffer_from);
    const bool ok_data = data && hooks::hook_function(data, reinterpret_cast<void*>(&hk_get_data), reinterpret_cast<void**>(&g_get_data));
    const bool ok_buffer =
        buffer && hooks::hook_function(buffer, reinterpret_cast<void*>(&hk_get_buffer), reinterpret_cast<void**>(&g_get_buffer));
    log("steam mouse: Raw Input reads hooked (%s GetRawInputData %s, %s GetRawInputBuffer %s)", data_from,
        ok_data ? "ok" : "failed", buffer_from, ok_buffer ? "ok" : "failed");
}

// Windows calls the most recently installed low-level hook first. DirectInput's exclusive mouse installs its own on every
// Acquire (DOOM 2016 acquires it when gameplay starts), and it took Steam's motion before ours dropped it: ours is
// installed again after each acquisition (and each time blocking starts).
constexpr UINT kReinstall = WM_APP + 1;
DWORD g_hook_thread = 0;

DWORD WINAPI hook_thread(void* param) {
    auto* ready = static_cast<std::atomic<DWORD>*>(param);
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // this thread's message queue
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&hook), &self);
    HHOOK h = SetWindowsHookExW(WH_MOUSE_LL, &hook, self, 0);
    ready->store(h ? 1 : 2 + GetLastError());
    if (!h) return 0;
    // Low-level hooks run in this loop. kReinstall: unhook and hook again, so ours is called first again.
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kReinstall && !msg.hwnd) {
            UnhookWindowsHookEx(h);
            h = SetWindowsHookExW(WH_MOUSE_LL, &hook, self, 0);
            if (!h) {
                log("steam mouse: low-level mouse hook lost on re-install (%lu)", GetLastError());
                return 0;
            }
            continue;
        }
        DispatchMessageW(&msg);
    }
    return 0;
}

bool g_hooked = false, g_hook_failed = false;
void set_state(const char* state, uint32_t view) {
    static std::string last;
    static uint32_t last_view = 0;
    if (last == state && last_view == view) return;
    last = state;
    last_view = view;
    log("steam mouse: %s (view %u)", state, view);
}

bool start_hook() {
    if (g_hooked || g_hook_failed) return g_hooked;
    static std::atomic<DWORD> ready{0};
    HANDLE thread = CreateThread(nullptr, 0, &hook_thread, &ready, 0, &g_hook_thread);
    if (thread) {
        while (!ready && WaitForSingleObject(thread, 1) == WAIT_TIMEOUT) {}
        CloseHandle(thread);
    }
    g_hooked = ready == 1;
    g_hook_failed = !g_hooked;
    if (g_hooked) hook_raw_input();
    if (g_hook_failed)
        log("steam mouse: low-level mouse hook not installed (%lu); Steam Input's mouse passes through",
            ready > 1 ? ready - 2 : DWORD(GetLastError()));
    return g_hooked;
}

}  // namespace

void frame(const Frame& f, double& pitch, double& yaw) {
    pitch = yaw = 0;
    // GyroLib's own row (input.steam_mouse, per view) is the setting: GyroLib shows it while its mouse bridge exists
    // (overlay.cpp keeps one, never fed, next to gyro4all.dll's panel). The view in front, else the last one seen.
    static uint32_t view = 1;
    if (f.view) view = f.view;
    char id[48];
    std::snprintf(id, sizeof(id), "context.%u.input.steam_mouse", view);
    double value = GL_STEAM_MOUSE_BLOCK;
    if (f.ctx && gl_setting_get(f.ctx, id, &value) != GL_OK) value = GL_STEAM_MOUSE_BLOCK;
    const int mode = int(value);
    const bool steam = f.pad && SDL_GetGamepadSteamHandle(f.pad) != 0;
    const bool block = mode != GL_STEAM_MOUSE_PASSTHROUGH && f.focused && !f.gyrolib_panel && steam;
    static bool blocking = false;
    static uint32_t acquires = 0;
    if (block && start_hook()) {
        const uint32_t n = dinputpad::mouse_acquires();
        if (!blocking || n != acquires) PostThreadMessageW(g_hook_thread, kReinstall, 0, 0);
        acquires = n;
        blocking = true;
        g_block_until.store(GetTickCount64() + kLeaseMs, std::memory_order_relaxed);
    } else {
        blocking = false;
        g_block_until.store(0, std::memory_order_relaxed);
    }
    // Convert: GyroLib's 0.05 degrees per count, right-positive yaw, up-positive pitch; only while the camera may turn
    // (menus and pause still block, and drop what was counted).
    g_convert.store(blocking && mode == GL_STEAM_MOUSE_CONVERT, std::memory_order_relaxed);
    const long long x = g_count_x.exchange(0), y = g_count_y.exchange(0);
    if (blocking && mode == GL_STEAM_MOUSE_CONVERT && f.camera) {
        yaw = double(x) * 0.05;
        pitch = -double(y) * 0.05;
    }
    char state[96];
    std::snprintf(state, sizeof(state), "%s",
                  f.gyrolib_panel ? "left to GyroLib's own option (DX12 panel)"
                  : !steam        ? "passes through (the controller isn't under Steam Input)"
                  : g_hook_failed ? "passes through (no low-level mouse hook)"
                  : mode == GL_STEAM_MOUSE_PASSTHROUGH ? "passes through (Pass through)"
                  : !block ? "passes through (no focus)"
                  : mode == GL_STEAM_MOUSE_CONVERT ? "converted (Convert movement)"
                                                   : "blocked (Block)");
    set_state(state, view);
    // How much was dropped, every 10 s while anything was.
    static ULONGLONG next = 0;
    if (const ULONGLONG now = GetTickCount64(); now >= next) {
        next = now + 10000;
        const unsigned n = g_blocked.exchange(0), raw = g_zeroed.exchange(0);
        if (n || raw)
            log("steam mouse: in the last 10 s, %u injected mouse motions blocked, %u injected Raw Input packets emptied", n, raw);
    }
}

}  // namespace gyro4all::steammouse
