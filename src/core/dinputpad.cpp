#include "dinputpad.hpp"

#include "hooks.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "vtable_hook.hpp"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <intrin.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace gyro4all::dinputpad {
namespace {

using Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, LPUNKNOWN);
using GetCapabilities_t = HRESULT(STDMETHODCALLTYPE*)(void*, LPDIDEVCAPS);
using GetProperty_t = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, LPDIPROPHEADER);
using GetDeviceState_t = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);
using GetDeviceData_t = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using Acquire_t = HRESULT(STDMETHODCALLTYPE*)(void*);
using SetCooperativeLevel_t = HRESULT(STDMETHODCALLTYPE*)(void*, HWND, DWORD);

// COM vtable slots: IDirectInput8 CreateDevice; IDirectInputDevice8 (A and W alike) GetCapabilities, GetProperty,
// Acquire, Unacquire, GetDeviceState, GetDeviceData, SetCooperativeLevel.
constexpr int kCreateDevice = 3, kGetCapabilities = 3, kGetProperty = 5, kAcquire = 7, kUnacquire = 8,
              kGetDeviceState = 9, kGetDeviceData = 10, kSetCooperativeLevel = 13;

Create_t g_create = nullptr;
VtableHook g_create_device, g_get_state, g_get_data, g_acquire, g_cooperative;
std::atomic<ReadFn> g_on_read{nullptr};
std::atomic<CreateFn> g_on_create{nullptr};

// The game's game-controller devices, each with the state it read last while the controller was at rest.
struct Pad {
    std::atomic<void*> device{nullptr};
    uint32_t id = 0;  // vendor << 16 | product, 0 unknown
    std::atomic<bool> read{false};
    std::mutex mutex;
    DWORD rest_size = 0;
    uint8_t rest[512] = {};
    bool fallback_logged = false;
    bool ranges_read = false;  // the game's Z/Rz ranges, read on the first stick fallback
    LONG z_min = 0, z_max = 65535, rz_min = 0, rz_max = 65535;
};
constexpr int kMaxPads = 16;
Pad g_pads[kMaxPads];

// The game's DirectInput mice and keyboards: idle while the settings panel captures them (DOOM 2016 reads both this
// way, so the panel's clicks and the mouse's motion also turned and fired in the game behind it). An exclusive one is
// also let go (Unacquire) while captured: Windows neither moves the cursor nor sends mouse messages while a game holds
// the mouse exclusively, so the panel saw no motion (DOOM 2016 on AMD, in game); it is acquired again on the game's
// first read after the panel lets go. Only on the game's own thread, inside its reads and Acquire calls.
struct Desktop {
    std::atomic<void*> device{nullptr};
    bool mouse = false;
    std::atomic<DWORD> level{0};          // the game's SetCooperativeLevel flags (0 = not set yet)
    std::atomic<bool> released{false};    // unacquired by us while captured
};
constexpr int kMaxDesktop = 8;
Desktop g_desktop[kMaxDesktop];
std::atomic<bool> g_capture_mouse{false}, g_capture_keyboard{false};
std::atomic<uint32_t> g_desktop_idled{0};

std::atomic<bool> g_neutral{false};
std::atomic<float> g_stick_x{0}, g_stick_y{0};
std::atomic<bool> g_rest{false};  // GyroLib's selected controller has been at rest for a few frames
int g_rest_frames = 0;
std::atomic<uint32_t> g_idled{0};

void* method(void* object, int slot) { return (*static_cast<void***>(object))[slot]; }

// The module is Gyro4All, GyroLib or GyroLib's SDL3.dll.
bool ours(const void* address) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(address), &m);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ours), &self);
    return m && (m == self || m == GetModuleHandleW(L"gyrolib.dll") || m == GetModuleHandleW(L"SDL3.dll"));
}

Desktop* find_desktop(void* device) {
    for (Desktop& d : g_desktop)
        if (d.device.load(std::memory_order_acquire) == device) return &d;
    return nullptr;
}

bool captured(const Desktop& d) {
    return (d.mouse ? g_capture_mouse : g_capture_keyboard).load(std::memory_order_relaxed);
}

const char* desktop_name(const Desktop& d) { return d.mouse ? "mouse" : "keyboard"; }

// On the game's read of a mouse or keyboard: true while an exclusive one is let go for the panel (the read returns
// idle without reaching DirectInput). The first read after the panel lets go acquires it again.
bool let_go(void* device, Desktop& d) {
    const bool exclusive = (d.level.load(std::memory_order_relaxed) & DISCL_EXCLUSIVE) != 0;
    if (exclusive && captured(d)) {
        if (!d.released.exchange(true)) {
            const HRESULT hr = reinterpret_cast<Acquire_t>(method(device, kUnacquire))(device);
            log("dinputpad: the game's exclusive DirectInput %s let go while the settings panel captures it (0x%08lX)",
                desktop_name(d), static_cast<unsigned long>(hr));
        }
        return true;
    }
    if (d.released.exchange(false)) {
        auto acquire = reinterpret_cast<Acquire_t>(g_acquire.original_for(device));
        if (!acquire) acquire = reinterpret_cast<Acquire_t>(method(device, kAcquire));
        const HRESULT hr = acquire(device);
        log("dinputpad: the game's exclusive DirectInput %s acquired again (0x%08lX)", desktop_name(d),
            static_cast<unsigned long>(hr));
    }
    return false;
}

// Buffered events of a captured mouse or keyboard: only releases go through (a button or key held when the panel
// opened is let go in the game too); presses and motion are dropped. Both formats start with dwOfs, dwData.
void keep_releases(const Desktop& d, DWORD object_size, LPDIDEVICEOBJECTDATA data, LPDWORD count) {
    auto* bytes = reinterpret_cast<uint8_t*>(data);
    DWORD kept = 0;
    for (DWORD i = 0; i < *count; ++i) {
        const auto* e = reinterpret_cast<const DIDEVICEOBJECTDATA*>(bytes + size_t(i) * object_size);
        const bool button = !d.mouse || (e->dwOfs >= DIMOFS_BUTTON0 && e->dwOfs <= DIMOFS_BUTTON7);
        if (!button || (e->dwData & 0x80)) continue;
        if (kept != i) std::memmove(bytes + size_t(kept) * object_size, e, object_size);
        ++kept;
    }
    g_desktop_idled.fetch_add(*count - kept, std::memory_order_relaxed);
    *count = kept;
}

Pad* find_pad(void* device) {
    for (Pad& p : g_pads)
        if (p.device.load(std::memory_order_acquire) == device) return &p;
    return nullptr;
}

void note_read(Pad& pad, const char* how) {
    if (pad.read.exchange(true)) return;
    if (ReadFn fn = g_on_read.load()) fn(pad.id, how);
}

// Where the game reads its controllers: each new call site and buffer is logged (up to kReadsLogged), and the last
// GetDeviceState buffer is kept for WriteWatch's "di" address (following the stick into the game's code).
struct Read {
    const void* caller;
    const void* buffer;
};
constexpr int kReadsLogged = 8;
std::mutex g_reads_mutex;
Read g_reads[kReadsLogged];
int g_read_count = 0;
std::atomic<uintptr_t> g_state_buffer{0};
std::atomic<uint32_t> g_state_size{0};

void note_call(const Pad& pad, bool state, const void* caller, const void* buffer, DWORD size) {
    if (state && buffer) {
        g_state_buffer.store(reinterpret_cast<uintptr_t>(buffer), std::memory_order_relaxed);
        g_state_size.store(size, std::memory_order_relaxed);
    }
    std::lock_guard lock(g_reads_mutex);
    for (int i = 0; i < g_read_count; ++i)
        if (g_reads[i].caller == caller && g_reads[i].buffer == buffer) return;
    if (g_read_count == kReadsLogged) return;
    g_reads[g_read_count++] = {caller, buffer};
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    const auto at = reinterpret_cast<uintptr_t>(buffer);
    const char* format = !state                        ? " per event"
                         : size == sizeof(DIJOYSTATE)  ? ", DIJOYSTATE: right stick lZ +0x8, lRz +0x14"
                         : size == sizeof(DIJOYSTATE2) ? ", DIJOYSTATE2: right stick lZ +0x8, lRz +0x14"
                                                       : ", a custom data format";
    log("dinputpad: the game reads controller %04X:%04X with %s (%lu bytes%s) into 0x%llX (%s), called from %s "
        "(thread %lu)%s",
        pad.id >> 16, pad.id & 0xFFFF, state ? "GetDeviceState" : "GetDeviceData", size, format, (unsigned long long)at,
        at >= low && at < high ? "on the caller's stack" : mem::module_offset(at).c_str(),
        mem::module_offset(reinterpret_cast<uintptr_t>(caller)).c_str(), GetCurrentThreadId(),
        g_read_count == kReadsLogged ? "; further call sites not logged" : "");
}

// Adds the stick fallback's deflection to the right stick (Z right, Rz down), within the ranges the game set.
void add_stick(void* device, Pad& pad, DWORD size, DIJOYSTATE* js, float x, float y) {
    if (!pad.ranges_read) {
        pad.ranges_read = true;
        const auto get = reinterpret_cast<GetProperty_t>(method(device, kGetProperty));
        const struct { DWORD offset; LONG *lo, *hi; } axes[] = {{DIJOFS_Z, &pad.z_min, &pad.z_max},
                                                                 {DIJOFS_RZ, &pad.rz_min, &pad.rz_max}};
        for (const auto& a : axes) {
            DIPROPRANGE range{};
            range.diph.dwSize = sizeof(range);
            range.diph.dwHeaderSize = sizeof(range.diph);
            range.diph.dwObj = a.offset;
            range.diph.dwHow = DIPH_BYOFFSET;
            if (SUCCEEDED(get(device, DIPROP_RANGE, &range.diph))) *a.lo = range.lMin, *a.hi = range.lMax;
        }
        log("dinputpad: stick fallback on the game's right stick (%lu-byte state, Z %ld..%ld, Rz %ld..%ld)", size,
            pad.z_min, pad.z_max, pad.rz_min, pad.rz_max);
    }
    const auto add = [](LONG& v, LONG lo, LONG hi, float d) {
        v = LONG(std::clamp(v + d * (double(hi) - lo) / 2, double(lo), double(hi)));
    };
    add(js->lZ, pad.z_min, pad.z_max, x);
    add(js->lRz, pad.rz_min, pad.rz_max, -y);
}

HRESULT STDMETHODCALLTYPE hk_get_state(void* device, DWORD size, LPVOID data) {
    const void* caller = _ReturnAddress();
    auto real = reinterpret_cast<GetDeviceState_t>(g_get_state.original_for(device));
    Desktop* desktop = find_desktop(device);
    if (desktop && let_go(device, *desktop)) {
        if (data) std::memset(data, 0, size);
        g_desktop_idled.fetch_add(1, std::memory_order_relaxed);
        return DI_OK;
    }
    const HRESULT hr = real ? real(device, size, data) : E_FAIL;
    if (const Desktop* d = desktop) {
        // Immediate state: nothing pressed, no motion (both formats are all relative axes and buttons).
        if (SUCCEEDED(hr) && data && captured(*d)) {
            std::memset(data, 0, size);
            g_desktop_idled.fetch_add(1, std::memory_order_relaxed);
        }
        return hr;
    }
    Pad* pad = find_pad(device);
    if (!pad) return hr;
    note_read(*pad, "GetDeviceState");
    if (FAILED(hr) || !data) return hr;
    note_call(*pad, true, caller, data, size);
    std::lock_guard lock(pad->mutex);
    if (!g_neutral.load(std::memory_order_relaxed)) {
        if (g_rest.load(std::memory_order_relaxed) && size <= sizeof(pad->rest)) {
            std::memcpy(pad->rest, data, size);
            pad->rest_size = size;
        }
        const float x = g_stick_x.load(std::memory_order_relaxed), y = g_stick_y.load(std::memory_order_relaxed);
        if ((x != 0 || y != 0) && (size == sizeof(DIJOYSTATE) || size == sizeof(DIJOYSTATE2)))
            add_stick(device, *pad, size, static_cast<DIJOYSTATE*>(data), x, y);
        return hr;
    }
    ++g_idled;
    if (pad->rest_size == size) {
        std::memcpy(data, pad->rest, size);
        return hr;
    }
    // No rest state yet: the standard joystick formats without buttons and with the hats centred (sticks unchanged).
    if (size == sizeof(DIJOYSTATE) || size == sizeof(DIJOYSTATE2)) {
        auto* js = static_cast<DIJOYSTATE*>(data);
        std::fill(std::begin(js->rgdwPOV), std::end(js->rgdwPOV), DWORD(-1));
        std::memset(js->rgbButtons, 0, sizeof(js->rgbButtons));
        if (size == sizeof(DIJOYSTATE2)) std::memset(static_cast<DIJOYSTATE2*>(data)->rgbButtons, 0, 128);
    }
    if (!pad->fallback_logged) {
        pad->fallback_logged = true;
        log("dinputpad: no rest state of the game's DirectInput controller yet (%lu-byte state): only its buttons cleared",
            size);
    }
    return hr;
}

// Buffered input: none while the panel has the controller (the events are read and dropped).
HRESULT STDMETHODCALLTYPE hk_get_data(void* device, DWORD object_size, LPDIDEVICEOBJECTDATA data, LPDWORD count,
                                      DWORD flags) {
    const void* caller = _ReturnAddress();
    auto real = reinterpret_cast<GetDeviceData_t>(g_get_data.original_for(device));
    Desktop* desktop = find_desktop(device);
    if (desktop && let_go(device, *desktop)) {
        if (count) *count = 0;
        return DI_OK;
    }
    const HRESULT hr = real ? real(device, object_size, data, count, flags) : E_FAIL;
    if (const Desktop* d = desktop) {
        if (SUCCEEDED(hr) && data && count && object_size >= 2 * sizeof(DWORD) && captured(*d))
            keep_releases(*d, object_size, data, count);
        return hr;
    }
    Pad* pad = find_pad(device);
    if (!pad) return hr;
    note_read(*pad, "GetDeviceData");
    if (SUCCEEDED(hr) && data) note_call(*pad, false, caller, data, object_size);
    if (SUCCEEDED(hr) && count && !(flags & DIGDD_PEEK) && g_neutral.load(std::memory_order_relaxed)) *count = 0;
    return hr;
}

// The game acquiring a mouse or keyboard we let go for the panel: left unacquired, reported done.
HRESULT STDMETHODCALLTYPE hk_acquire(void* device) {
    auto real = reinterpret_cast<Acquire_t>(g_acquire.original_for(device));
    if (const Desktop* d = find_desktop(device); d && d->released.load() && captured(*d)) return DI_OK;
    return real ? real(device) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE hk_cooperative(void* device, HWND window, DWORD flags) {
    auto real = reinterpret_cast<SetCooperativeLevel_t>(g_cooperative.original_for(device));
    const HRESULT hr = real ? real(device, window, flags) : E_FAIL;
    if (Desktop* d = find_desktop(device); d && SUCCEEDED(hr) && d->level.exchange(flags) != flags)
        log("dinputpad: the game's DirectInput %s: cooperative level 0x%lX (%s, %s)", desktop_name(*d),
            static_cast<unsigned long>(flags), flags & DISCL_EXCLUSIVE ? "exclusive" : "non-exclusive",
            flags & DISCL_BACKGROUND ? "background" : "foreground");
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_create_device(void* input, REFGUID guid, void** out, LPUNKNOWN outer) {
    const void* caller = _ReturnAddress();
    auto real = reinterpret_cast<CreateDevice_t>(g_create_device.original_for(input));
    const HRESULT hr = real ? real(input, guid, out, outer) : E_FAIL;
    void* device = SUCCEEDED(hr) && out ? *out : nullptr;
    if (!device || ours(caller)) return hr;
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    if (FAILED(reinterpret_cast<GetCapabilities_t>(method(device, kGetCapabilities))(device, &caps))) return hr;
    const BYTE type = GET_DIDEVICE_TYPE(caps.dwDevType);
    if (type == DI8DEVTYPE_MOUSE || type == DI8DEVTYPE_KEYBOARD) {
        g_get_state.patch(device, kGetDeviceState, reinterpret_cast<void*>(&hk_get_state));
        g_get_data.patch(device, kGetDeviceData, reinterpret_cast<void*>(&hk_get_data));
        g_acquire.patch(device, kAcquire, reinterpret_cast<void*>(&hk_acquire));
        g_cooperative.patch(device, kSetCooperativeLevel, reinterpret_cast<void*>(&hk_cooperative));
        for (Desktop& d : g_desktop) {
            if (d.device.load() == device) return hr;
            void* expected = nullptr;
            if (d.device.compare_exchange_strong(expected, device)) {
                d.mouse = type == DI8DEVTYPE_MOUSE;
                log("dinputpad: the game opens a DirectInput %s: idle while the settings panel captures it",
                    d.mouse ? "mouse" : "keyboard");
                return hr;
            }
        }
        return hr;
    }
    if (type < DI8DEVTYPE_JOYSTICK || type > DI8DEVTYPE_1STPERSON) return hr;  // other devices
    DIPROPDWORD vidpid{};
    vidpid.diph.dwSize = sizeof(vidpid);
    vidpid.diph.dwHeaderSize = sizeof(vidpid.diph);
    vidpid.diph.dwHow = DIPH_DEVICE;
    const uint32_t id =
        SUCCEEDED(reinterpret_cast<GetProperty_t>(method(device, kGetProperty))(device, DIPROP_VIDPID, &vidpid.diph))
            ? (vidpid.dwData & 0xFFFF) << 16 | vidpid.dwData >> 16
            : 0;
    g_get_state.patch(device, kGetDeviceState, reinterpret_cast<void*>(&hk_get_state));
    g_get_data.patch(device, kGetDeviceData, reinterpret_cast<void*>(&hk_get_data));
    for (Pad& p : g_pads) {
        if (p.device.load() == device) return hr;
        void* expected = nullptr;
        if (p.device.compare_exchange_strong(expected, device)) {
            p.id = id;
            static std::atomic<int> logged{0};
            if (logged++ < 4)
                log("dinputpad: the game opens a DirectInput game controller (%04X:%04X, type 0x%02X, %lu axes, %lu "
                    "buttons): idle while GyroLib's panel has the controller",
                    id >> 16, id & 0xFFFF, unsigned(type), caps.dwAxes, caps.dwButtons);
            return hr;
        }
    }
    return hr;
}

HRESULT WINAPI hk_create(HINSTANCE instance, DWORD version, REFIID iid, LPVOID* out, LPUNKNOWN outer) {
    if (CreateFn fn = g_on_create.load()) fn();
    // Import slots: the original as patched, else (a delay-load not resolved yet) from dinput8.dll itself.
    Create_t real = reinterpret_cast<Create_t>(InterlockedCompareExchangePointer(reinterpret_cast<void**>(&g_create), nullptr, nullptr));
    if (!real)
        if (HMODULE m = LoadLibraryW(L"dinput8.dll"))
            real = reinterpret_cast<Create_t>(GetProcAddress(m, "DirectInput8Create"));
    const HRESULT hr = real ? real(instance, version, iid, out, outer) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) g_create_device.patch(*out, kCreateDevice, reinterpret_cast<void*>(&hk_create_device));
    return hr;
}

}  // namespace

void install() {
    // Protected exes: inline on dinput8.dll's export (loaded first; it is a system DLL), else the game modules' imports.
    if (hooks::inline_mode()) LoadLibraryW(L"dinput8.dll");
    const bool ok = hooks::add(hooks::inline_mode() ? "dinput8.dll" : nullptr, "DirectInput8Create",
                               reinterpret_cast<void*>(&hk_create), reinterpret_cast<void**>(&g_create));
    log("dinputpad: DirectInput8Create hook %s", ok ? "installed" : "registered (no game module imports it yet)");
}

void on_read(ReadFn fn) { g_on_read = fn; }

void on_create(CreateFn fn) { g_on_create = fn; }

void frame(bool neutral, SDL_Gamepad* pad) {
    bool rest = pad != nullptr;
    for (int b = 0; rest && b < SDL_GAMEPAD_BUTTON_COUNT; ++b) rest = !SDL_GetGamepadButton(pad, SDL_GamepadButton(b));
    for (int a = 0; rest && a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
        const int v = SDL_GetGamepadAxis(pad, SDL_GamepadAxis(a));
        const bool trigger = a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        rest = trigger ? v < 2000 : std::abs(v) < 4000;
    }
    // A few frames at rest, so the game's read in between saw the controller at rest too.
    g_rest_frames = rest ? std::min(g_rest_frames + 1, 100) : 0;
    g_rest = g_rest_frames >= 5;
    const bool any = std::any_of(std::begin(g_pads), std::end(g_pads), [](const Pad& p) { return p.device.load() != nullptr; });
    if (g_neutral.exchange(neutral) != neutral && any)
        log("dinputpad: the game's DirectInput controller %s (%u states idled so far)",
            neutral ? "reads idle (GyroLib's panel has the controller)" : "passed through again", g_idled.load());
}

void set_capture(bool mouse, bool keyboard) {
    const bool any = std::any_of(std::begin(g_desktop), std::end(g_desktop),
                                 [](const Desktop& d) { return d.device.load() != nullptr; });
    const bool was_mouse = g_capture_mouse.exchange(mouse), was_keyboard = g_capture_keyboard.exchange(keyboard);
    if (any && (was_mouse != mouse || was_keyboard != keyboard)) {
        std::string levels;  // what the game asked for each device (0 = not set yet)
        for (const Desktop& d : g_desktop)
            if (d.device.load()) {
                char one[48];
                std::snprintf(one, sizeof(one), "%s%s 0x%lX%s", levels.empty() ? "" : ", ", desktop_name(d),
                              static_cast<unsigned long>(d.level.load()), d.released.load() ? " let go" : "");
                levels += one;
            }
        log("dinputpad: the game's DirectInput mouse %s, keyboard %s (%u reads idled so far; cooperative levels: %s)",
            mouse ? "idle" : "passed through", keyboard ? "idle" : "passed through", g_desktop_idled.load(),
            levels.c_str());
    }
}

void set_stick(float x, float y) {
    g_stick_x.store(x, std::memory_order_relaxed);
    g_stick_y.store(y, std::memory_order_relaxed);
}

bool last_state(uintptr_t& buffer, uint32_t& size) {
    buffer = g_state_buffer.load(std::memory_order_relaxed);
    size = g_state_size.load(std::memory_order_relaxed);
    return buffer != 0;
}

}  // namespace gyro4all::dinputpad
