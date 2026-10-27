// UE3's controller list (MODLOG 2026-10-26, Dishonored): the Windows client keeps a list of joysticks, built when the
// viewport is created: every XInput slot whose XInputGetState answers becomes an entry, and each frame only the listed
// slots are polled. The controller Gyro4All shows a moment later is never listed, so the game never asks for it, and
// Dishonored didn't build the list again for a posted WM_DEVICECHANGE either (its window procedure has a case for it,
// but nothing came of it, posted from Gyro4All or from outside). So Gyro4All runs that enumeration itself when the
// controller comes or goes, on the game thread, like the game's own calls. It adds the slots that answer and drops the
// ones that stopped.
//
// Found from the import, no exe addresses: the exe's XInputGetState import slot -> `jmp [rip+slot]` thunks or direct
// `call [rip+slot]` -> the call that is followed, within 0x30 bytes, by the 4-slot loop's `cmp r32,4` and a backward
// `jl`, in a function starting at most 0x100 bytes before it (the per-frame poll is deep inside a large function).
#include "ue3_xinput.hpp"

#include "log.hpp"
#include "mem.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <vector>

namespace gyro4all::unreal3 {
namespace {

using Enumerate_t = void (*)(void*);
Enumerate_t g_enumerate = nullptr;
bool g_looked = false;

const IMAGE_NT_HEADERS64* nt_headers(uintptr_t base) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
}

// The exe's import slots of XInputGetState: by name, or ordinal 2 of xinput1_3/1_4 (the Windows SDK's xinput.lib).
std::set<uintptr_t> import_slots(uintptr_t base) {
    std::set<uintptr_t> slots;
    const IMAGE_DATA_DIRECTORY& dir = nt_headers(base)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return slots;
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        const char* dll = reinterpret_cast<const char*>(base + d->Name);
        if (_strnicmp(dll, "xinput", 6) != 0 || !d->FirstThunk) continue;
        const bool by_ordinal = _strnicmp(dll, "xinput1_3", 9) == 0 || _strnicmp(dll, "xinput1_4", 9) == 0;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        for (size_t i = 0; names[i].u1.AddressOfData; ++i) {
            const ULONGLONG v = names[i].u1.AddressOfData;
            const bool match = IMAGE_SNAP_BY_ORDINAL64(v)
                                   ? by_ordinal && IMAGE_ORDINAL64(v) == 2
                                   : std::strcmp(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + v)->Name, "XInputGetState") == 0;
            if (match) slots.insert(base + d->FirstThunk + i * 8);
        }
    }
    return slots;
}

// Addresses of the RIP-relative branches in the exe's code to any of targets: `FF 25|FF 15 rel32` (indirect, rel_at 2)
// or `E8 rel32` (direct, rel_at 1).
std::vector<uintptr_t> branches(uintptr_t base, const std::set<uintptr_t>& targets, uint8_t op0, uint8_t op1, size_t rel_at) {
    std::vector<uintptr_t> out;
    const IMAGE_NT_HEADERS64* nt = nt_headers(base);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    const size_t len = rel_at + 4;
    std::vector<uint8_t> buf(0x10000 + len);
    for (WORD n = 0; n < nt->FileHeader.NumberOfSections; ++n, ++s) {
        if (!(s->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uintptr_t begin = base + s->VirtualAddress, end = begin + s->Misc.VirtualSize;
        for (uintptr_t at = begin; at + len <= end; at += 0x10000) {
            const size_t n_bytes = size_t(std::min<uintptr_t>(end - at, buf.size()));
            if (!mem::guarded_copy(buf.data(), at, n_bytes)) continue;
            for (size_t i = 0; i + len <= n_bytes && i < 0x10000; ++i) {
                if (buf[i] != op0 || (rel_at == 2 && buf[i + 1] != op1)) continue;
                int32_t rel = 0;
                std::memcpy(&rel, &buf[i + rel_at], 4);
                if (targets.count(at + i + len + rel)) out.push_back(at + i);
            }
        }
    }
    return out;
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
        f = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + ((header[2] + 1) & ~1) * 2);
    }
    return 0;
}

// After the call ending at `after`: `83 F8+r 04` (cmp r32,4) and, within 8 bytes, `7C rel8<0` or `0F 8C rel32<0`.
bool slot_loop_after(uintptr_t after) {
    uint8_t b[0x30 + 14] = {};
    if (!mem::guarded_copy(b, after, sizeof b)) return false;
    for (size_t i = 0; i < 0x30; ++i) {
        if (b[i] != 0x83 || (b[i + 1] & 0xF8) != 0xF8 || b[i + 2] != 4) continue;
        for (size_t j = i + 3; j < i + 11; ++j) {
            if (b[j] == 0x7C && int8_t(b[j + 1]) < 0) return true;
            int32_t rel = 0;
            std::memcpy(&rel, &b[j + 2], 4);
            if (b[j] == 0x0F && b[j + 1] == 0x8C && rel < 0) return true;
        }
    }
    return false;
}

void find_enumeration() {
    g_looked = true;
    const ULONGLONG start = GetTickCount64();
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const std::set<uintptr_t> slots = import_slots(base);
    if (slots.empty()) {
        log("ue3: controller list: the exe doesn't import XInputGetState; the game is left to look for controllers itself");
        return;
    }
    std::vector<std::pair<uintptr_t, size_t>> calls;  // call site, instruction length
    std::set<uintptr_t> thunks;
    for (uintptr_t a : branches(base, slots, 0xFF, 0x25, 2)) thunks.insert(a);
    for (uintptr_t a : branches(base, slots, 0xFF, 0x15, 2)) calls.push_back({a, 6});
    if (!thunks.empty())
        for (uintptr_t a : branches(base, thunks, 0xE8, 0, 1)) calls.push_back({a, 5});
    std::set<uintptr_t> found;
    for (const auto& [site, len] : calls) {
        const uintptr_t f = function_start(site);
        if (f && site - f <= 0x100 && slot_loop_after(site + len)) found.insert(f);
    }
    if (found.size() != 1) {
        log("ue3: controller list: %zu candidates for its enumeration (%zu calls of XInputGetState, %zu thunks, %llu ms); "
            "the game is left to look for controllers itself",
            found.size(), calls.size(), thunks.size(), GetTickCount64() - start);
        return;
    }
    g_enumerate = reinterpret_cast<Enumerate_t>(*found.begin());
    log("ue3: controller list: enumeration at %s (found from %zu calls of XInputGetState, %llu ms)",
        mem::module_offset(*found.begin()).c_str(), calls.size(), GetTickCount64() - start);
}

// No C++ objects here (__try).
bool run_enumeration() {
    __try {
        g_enumerate(nullptr);  // static in UE3; a null `this` would fault at once instead of using a stray one
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

void recheck_controllers() {
    if (!g_looked) find_enumeration();
    if (!g_enumerate) return;
    if (run_enumeration()) {
        log("ue3: controller list: the game looked for controllers again");
    } else {
        log("ue3: controller list: its enumeration faulted; not called again");
        g_enumerate = nullptr;
    }
}

}  // namespace gyro4all::unreal3
