// Frostbite's XInput devices (Battlefield 4; MODLOG 2026-10-21): the game creates an XInput device only for a
// controller that DirectInput lists and that Microsoft's WMI IsXInputDevice sample (DeviceID with "IG_" and the
// DirectInput product's VID/PID) calls XInput; anything else becomes a DirectInput joystick. The controller Gyro4All
// shows as XInput is neither, so the game would never poll it. This hooks the game's check so it says yes for the
// controllers Gyro4All translates (Sony, Nintendo): the game then makes it an XInput device (slot 0 with no real Xbox
// pad, as the translated one) instead of a joystick, and polls it through the hooked XInputGetState.
//
// Found from the XInput import, no exe addresses: the exe's import slot of XInputGetCapabilities -> the code that
// calls it (through a jmp thunk or directly) -> the functions containing those calls (unwind table) -> their callers;
// in the enumeration callback, the device constructor's call is preceded by `call <check>; test al,al; je`. An exe
// without that import (Battlefield Hardline) is searched by the sample's wide "IG_" string: the one function that
// loads it (lea) is the check. Installed
// from the game's first DirectInput8Create (dinputpad::on_create): its code is unpacked then and it hasn't listed any
// device yet.
#include "fb_xinput.hpp"

#include "fb_types.hpp"

#include "hooks.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "xinput.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

namespace gyro4all::fb {

namespace {

using Check_t = bool (*)(const GUID* product);
Check_t g_check = nullptr;
std::atomic<bool> g_tried{false};

constexpr uint16_t kSony = 0x054C, kNintendo = 0x057E;

// DirectInput's product GUID for a HID device: Data1 = MAKELONG(vendor, product).
bool hk_check(const GUID* product) {
    const bool real = g_check(product);
    if (real || !product || !xinput::translates()) return real;
    const uint16_t vendor = LOWORD(product->Data1), id = HIWORD(product->Data1);
    if (vendor != kSony && vendor != kNintendo) return false;
    xinput::listed_as_xinput(vendor, id);
    return true;
}

// The start of the function containing code address a (the primary entry of chained unwind info), or 0.
uintptr_t function_start(uintptr_t a) {
    DWORD64 base = 0;
    const RUNTIME_FUNCTION* f = RtlLookupFunctionEntry(a, &base, nullptr);
    for (int depth = 0; f && depth < 8; ++depth) {
        const auto* info = reinterpret_cast<const uint8_t*>(base + (f->UnwindData & ~1u));
        uint8_t header[4] = {};
        if (!mem::guarded_copy(header, reinterpret_cast<uintptr_t>(info), 4)) return 0;
        if (!((header[0] >> 3) & UNW_FLAG_CHAININFO)) return base + f->BeginAddress;
        // The chained (parent) entry follows the unwind codes, which are padded to an even count.
        f = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + ((header[2] + 1) & ~1) * 2);
    }
    return 0;
}

bool is_function(uintptr_t a) { return a && function_start(a) == a; }

// Before the call at site: `E8 rel32`, then within 16 bytes `84 C0; 74 rel8 | 0F 84 rel32` (a bool check and a jump
// if false; the compiler may put other instructions in between, as in Battlefield 4: `call; lea rdx,[..]; test al,al;
// je`), within 0x80 bytes. The nearest such call's function, or 0.
uintptr_t check_before(uintptr_t site) {
    uint8_t code[0x80] = {};
    const uintptr_t from = site - sizeof code;
    if (!mem::guarded_copy(code, from, sizeof code)) return 0;
    for (size_t i = sizeof code - 9; i-- > 0;) {
        if (code[i] != 0xE8) continue;
        bool tested = false;
        for (size_t j = i + 5; j < i + 5 + 16 && j + 3 < sizeof code && !tested; ++j)
            tested = code[j] == 0x84 && code[j + 1] == 0xC0 &&
                     (code[j + 2] == 0x74 || (code[j + 2] == 0x0F && code[j + 3] == 0x84));
        if (!tested) continue;
        int32_t rel = 0;
        std::memcpy(&rel, code + i + 1, 4);
        const uintptr_t target = from + i + 5 + rel;
        if (is_function(target)) return target;
    }
    return 0;
}

// The image addresses of the wide, null-terminated text preceded by a null character (a string of its own).
std::vector<uintptr_t> wide_strings(const wchar_t* text) {
    std::vector<uint8_t> bytes(2, 0);
    for (const wchar_t* c = text; *c; ++c) bytes.push_back(static_cast<uint8_t>(*c)), bytes.push_back(0);
    bytes.push_back(0), bytes.push_back(0);
    std::vector<uintptr_t> out = find_pattern(bytes.data(), bytes.size());
    for (uintptr_t& a : out) a += 2;
    return out;
}

void hook_check(uintptr_t check, const char* found_from, ULONGLONG start) {
    const bool ok = hooks::hook_function(reinterpret_cast<void*>(check), reinterpret_cast<void*>(&hk_check),
                                         reinterpret_cast<void**>(&g_check));
    log("fb: XInput device check (IsXInputDevice) at %s %s: Sony and Nintendo controllers count as XInput devices "
        "(found from %s, %llu ms)",
        mem::module_offset(check).c_str(), ok ? "hooked" : "could NOT be hooked", found_from, GetTickCount64() - start);
}

// Retried on the next DirectInput8Create while the import slot isn't there (GyroLib's SDL may create DirectInput before
// the protector filled in the exe's imports).
void install() {
    static std::mutex mutex;  // the game's and SDL's DirectInput8Create may come from two threads
    std::lock_guard lock(mutex);
    if (g_tried || !xinput::translates()) return;
    const ULONGLONG start = GetTickCount64();
    std::vector<uintptr_t> exports;
    for (const char* dll : {"xinput9_1_0.dll", "xinput1_3.dll", "xinput1_4.dll"})
        if (HMODULE m = GetModuleHandleA(dll))
            if (FARPROC f = GetProcAddress(m, "XInputGetCapabilities")) exports.push_back(reinterpret_cast<uintptr_t>(f));
    const std::vector<uintptr_t> slots = find_in_image(exports);
    if (slots.empty()) {
        // No import (Battlefield Hardline resolves XInput at run time): the check from its "IG_" string instead.
        std::set<uintptr_t> checks;
        for (const auto& [site, text] : find_lea_refs(wide_strings(L"IG_")))
            if (const uintptr_t f = function_start(site)) checks.insert(f);
        if (checks.size() == 1) {
            g_tried = true;
            hook_check(*checks.begin(), "its \"IG_\" string", start);
            return;
        }
        static int attempts = 0;
        log("fb: XInput device check: no import of XInputGetCapabilities in the exe and %zu functions using an \"IG_\" "
            "string (yet; attempt %d, XInput DLLs loaded: %zu)", checks.size(), ++attempts, exports.size());
        return;
    }
    g_tried = true;
    // Calls to the import: [rip+slot] directly, or through `jmp [rip+slot]` thunks.
    std::vector<uintptr_t> targets = slots, thunks;
    for (const auto& [site, slot] : find_branches(slots)) {
        uint8_t op[2] = {};
        if (mem::guarded_copy(op, site, 2) && op[1] == 0x25) thunks.push_back(site);
    }
    targets.insert(targets.end(), thunks.begin(), thunks.end());
    std::set<uintptr_t> users;  // functions calling XInputGetCapabilities
    for (const auto& [site, target] : find_branches(targets)) {
        uint8_t op[2] = {};
        if (!mem::guarded_copy(op, site, 2)) continue;
        if (op[0] == 0xE8 || (op[0] == 0xFF && op[1] == 0x15))
            if (const uintptr_t f = function_start(site)) users.insert(f);
    }
    std::set<uintptr_t> checks;
    for (const auto& [site, user] : find_branches({users.begin(), users.end()})) {
        uint8_t op = 0;
        if (mem::guarded_copy(&op, site, 1) && op == 0xE8 && function_start(site))
            if (const uintptr_t check = check_before(site)) checks.insert(check);
    }
    if (checks.size() != 1) {
        log("fb: XInput device check: %zu candidates (%zu import slots, %zu thunks, %zu functions calling "
            "XInputGetCapabilities); not hooked (%llu ms)",
            checks.size(), slots.size(), thunks.size(), users.size(), GetTickCount64() - start);
        return;
    }
    char how[64];
    std::snprintf(how, sizeof how, "%zu functions calling XInputGetCapabilities", users.size());
    hook_check(*checks.begin(), how, start);
}

}  // namespace

void hook_xinput_device_check() { install(); }

}  // namespace gyro4all::fb
