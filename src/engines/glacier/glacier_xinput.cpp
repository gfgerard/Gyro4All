// Glacier's XInput devices (007 First Light, MODLOG 2026-10-29): the input manager (exe+0x994820) gets every
// controller DirectInput lists; PlayStation ones become Sony pad devices, and the rest an XInput device
// (ZDeviceGamepad_XInput_Windows) only if Microsoft's WMI IsXInputDevice sample (exe+0x994F40, the DirectInput
// product GUID in rdx; "IG_" in the WMI DeviceID and the GUID's VID/PID) says so; anything else is dropped. The new
// device takes the first slot from 0 whose XInputGetCapabilities reads a connected gamepad, once (none: slot -1, never
// polled). A Switch Pro controller Gyro4All shows as XInput was never polled: this hooks the check so it says yes for
// Nintendo controllers when the XInput translation is on, and the core reads it as connected in the slot it will get
// until the frame shows it (xinput::listed_as_xinput without waiting: the device list may be built on the frame's thread).
//
// Found from the sample's wide "IG_" string, no exe addresses: the functions that load it (lea) whose direct callers
// test the result (`call; test eax,eax | test al,al; je`, within 16 bytes); 007 First Light has a second copy whose caller
// doesn't. Installed from the game's first DirectInput8Create (dinputpad::on_create), before it lists any device.
#include "glacier_xinput.hpp"

#include "hooks.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "xinput.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

namespace gyro4all::glacier {

namespace {

using Check_t = int (*)(void* self, const GUID* product);
Check_t g_check = nullptr;
std::atomic<bool> g_tried{false};

constexpr uint16_t kNintendo = 0x057E;

// DirectInput's product GUID for a HID device: Data1 = MAKELONG(vendor, product).
int hk_check(void* self, const GUID* product) {
    const int real = g_check(self, product);
    if (real || !product || !xinput::translates()) return real;
    const uint16_t vendor = LOWORD(product->Data1), id = HIWORD(product->Data1);
    if (vendor != kNintendo) return 0;
    xinput::listed_as_xinput(vendor, id, false);
    return 1;
}

uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

// The exe's sections: (begin, end) of each, executable or not.
std::vector<mem::Range> exe_sections(bool code) {
    const uintptr_t base = exe_base();
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    std::vector<mem::Range> out;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
        if (((s->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) == code)
            out.push_back({base + s->VirtualAddress, base + s->VirtualAddress + s->Misc.VirtualSize});
    return out;
}

// Calls fn(address, bytes, length) on each chunk of the ranges, chunks overlapping by `overlap` bytes.
template <class Fn>
void each_chunk(const std::vector<mem::Range>& ranges, size_t overlap, Fn fn) {
    std::vector<uint8_t> chunk(1 << 20);
    for (const mem::Range& r : ranges)
        for (uintptr_t at = r.begin; at < r.end; at += chunk.size() - overlap) {
            const size_t len = std::min<uintptr_t>(chunk.size(), r.end - at);
            if (len > overlap && mem::guarded_copy(chunk.data(), at, len)) fn(at, chunk.data(), len);
            if (len < chunk.size()) break;
        }
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

// The wide text "IG_" as a string of its own (a null character before it, its own null after).
std::set<uintptr_t> ig_strings() {
    static constexpr uint8_t kText[] = {0, 0, 'I', 0, 'G', 0, '_', 0, 0, 0};
    std::set<uintptr_t> out;
    each_chunk(exe_sections(false), sizeof kText, [&](uintptr_t at, const uint8_t* bytes, size_t len) {
        for (size_t i = 0; i + sizeof kText <= len; ++i)
            if (!std::memcmp(bytes + i, kText, sizeof kText)) out.insert(at + i + 2);
    });
    return out;
}

// After a call, within 16 bytes: `test eax,eax` or `test al,al`, then a je (rel8 or rel32).
bool tests_result(const uint8_t* after, size_t n) {
    for (size_t j = 0; j + 4 <= n && j < 16; ++j)
        if ((after[j] == 0x85 || after[j] == 0x84) && after[j + 1] == 0xC0 &&
            (after[j + 2] == 0x74 || (after[j + 2] == 0x0F && after[j + 3] == 0x84)))
            return true;
    return false;
}

void install() {
    static std::mutex mutex;  // the game's and SDL's DirectInput8Create may come from two threads
    std::lock_guard lock(mutex);
    if (g_tried.exchange(true) || !xinput::translates()) return;
    const ULONGLONG start = GetTickCount64();
    const std::set<uintptr_t> strings = ig_strings();
    const std::vector<mem::Range> code = exe_sections(true);
    // Functions loading the string: lea r64,[rip+disp32] (48/4C 8D, ModRM mod 00 rm 101).
    std::set<uintptr_t> users;
    each_chunk(code, 7, [&](uintptr_t at, const uint8_t* b, size_t len) {
        for (size_t i = 0; i + 7 <= len; ++i) {
            if ((b[i] != 0x48 && b[i] != 0x4C) || b[i + 1] != 0x8D || (b[i + 2] & 0xC7) != 0x05) continue;
            int32_t disp = 0;
            std::memcpy(&disp, b + i + 3, 4);
            if (strings.count(at + i + 7 + disp))
                if (const uintptr_t f = function_start(at + i)) users.insert(f);
        }
    });
    // Of those, the ones a direct call's caller tests.
    std::set<uintptr_t> checks;
    if (!users.empty())
        each_chunk(code, 5 + 20, [&](uintptr_t at, const uint8_t* b, size_t len) {
            for (size_t i = 0; i + 5 + 20 <= len; ++i) {
                if (b[i] != 0xE8) continue;
                int32_t rel = 0;
                std::memcpy(&rel, b + i + 1, 4);
                const uintptr_t target = at + i + 5 + rel;
                if (users.count(target) && tests_result(b + i + 5, 20)) checks.insert(target);
            }
        });
    if (checks.size() != 1) {
        log("glacier: XInput device check (IsXInputDevice): %zu candidates (%zu \"IG_\" strings, %zu functions using "
            "them); not hooked (%llu ms)",
            checks.size(), strings.size(), users.size(), GetTickCount64() - start);
        return;
    }
    const uintptr_t check = *checks.begin();
    const bool ok = hooks::hook_function(reinterpret_cast<void*>(check), reinterpret_cast<void*>(&hk_check),
                                         reinterpret_cast<void**>(&g_check));
    log("glacier: XInput device check (IsXInputDevice) at %s %s: Nintendo controllers count as XInput devices (%llu ms)",
        mem::module_offset(check).c_str(), ok ? "hooked" : "could NOT be hooked", GetTickCount64() - start);
}

}  // namespace

void hook_xinput_device_check() { install(); }

}  // namespace gyro4all::glacier
