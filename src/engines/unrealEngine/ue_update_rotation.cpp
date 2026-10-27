#include "ue_update_rotation.hpp"

#include "hooks.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "ue_update_rotation_match.hpp"
#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

namespace gyro4all::unreal::update_rotation {
namespace {

using UpdateRotationFn = void (*)(void* controller, float delta_time);

// One hooked UpdateRotation per function (controller classes that share it share the hook). All on the game thread:
// the gyro is queued from the PeekMessageW pump and taken in UpdateRotation (the player tick).
struct Slot {
    uintptr_t function = 0;
    UpdateRotationFn original = nullptr;
};
constexpr int kSlots = 4;
Slot g_slots[kSlots];

// Classes already looked at (vtable -> its UpdateRotation, 0 = not found).
struct Known {
    uintptr_t vtable = 0, function = 0;
};
std::vector<Known> g_known;


// The queued turn, for one controller.
struct Pending {
    ue::Obj pc = 0;
    int32_t offset = -1, size = 0;
    double pitch = 0, yaw = 0;
    ULONGLONG since = 0;  // first queued and not taken yet
} g_pending;
constexpr ULONGLONG kStaleMs = 250;  // not taken for this long (the tick doesn't run): dropped, not saved for later

void take(void* controller) {
    if (ue::Obj(controller) != g_pending.pc || (g_pending.pitch == 0 && g_pending.yaw == 0)) return;
    ue::add_rotator(g_pending.pc + g_pending.offset, g_pending.size, g_pending.pitch, g_pending.yaw);
    g_pending.pitch = g_pending.yaw = 0;
    g_pending.since = 0;
}

template <int N>
void detour(void* controller, float delta_time) {
    take(controller);
    g_slots[N].original(controller, delta_time);
}

constexpr UpdateRotationFn kDetours[kSlots] = {detour<0>, detour<1>, detour<2>, detour<3>};

bool executable_image(uintptr_t p) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE && (mbi.Protect & exec);
}

std::string where(uintptr_t p) {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = L"";
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(p), &module) ||
        !GetModuleFileNameW(module, path, MAX_PATH))
        return std::format("0x{:X}", p);
    std::wstring name = path;
    name = name.substr(name.find_last_of(L"\\/") + 1);
    return std::format("{}+0x{:X}", narrow(name), p - uintptr_t(module));
}

// The controller's vtable entries whose function reads RotationInput first. Entries end at the first one that isn't
// code of a loaded module.
std::vector<std::pair<int, uintptr_t>> find(uintptr_t vtable, int32_t offset, int32_t size) {
    std::vector<std::pair<int, uintptr_t>> found;
    for (int i = 0; i < 2048; ++i) {
        uintptr_t fn = mem::rdp(vtable + uintptr_t(i) * 8, 1);
        if (!executable_image(fn)) break;
        uint8_t code[0x140];
        if (!mem::guarded_copy(code, fn, sizeof(code))) continue;
        if (code[0] == 0xE9) {  // an incremental-link thunk: its target
            fn += 5 + uintptr_t(intptr_t(*reinterpret_cast<int32_t*>(code + 1)));
            if (!executable_image(fn) || !mem::guarded_copy(code, fn, sizeof(code))) continue;
        }
        if (!reads_this_field_first(code, sizeof(code), offset, size)) continue;
        if (std::none_of(found.begin(), found.end(), [&](const auto& f) { return f.second == fn; })) found.push_back({i, fn});
    }
    return found;
}

// The hooked UpdateRotation for this controller's class, 0 if not found.
uintptr_t hooked(ue::Obj pc, int32_t offset, int32_t size) {
    const uintptr_t vtable = mem::rdp(pc);
    if (!vtable) return 0;
    for (const Known& k : g_known)
        if (k.vtable == vtable) return k.function;
    Known known{vtable, 0};
    const std::string cls = ue::name_of(ue::class_of(pc));
    const auto found = find(vtable, offset, size);
    if (found.size() != 1) {
        std::string list;
        for (const auto& [index, fn] : found) list += std::format(" [{}] {}", index, where(fn));
        log("update rotation: %s: %zu functions of its vtable read RotationInput (+0x%X) first%s: %s", cls.c_str(),
            found.size(), unsigned(offset), list.c_str(),
            found.empty() ? "UpdateRotation not found, the gyro can't use RotationInput"
                          : "not unique, the gyro can't use RotationInput");
    } else {
        const auto [index, fn] = found.front();
        Slot* slot = nullptr;
        int n = 0;
        for (; n < kSlots; ++n) {
            if (g_slots[n].function == fn) {
                known.function = fn;  // already hooked for another class
                break;
            }
            if (!g_slots[n].function) {
                slot = &g_slots[n];
                break;
            }
        }
        if (!known.function && slot) {
            const bool ok = hooks::hook_function(reinterpret_cast<void*>(fn), reinterpret_cast<void*>(kDetours[n]),
                                                 reinterpret_cast<void**>(&slot->original));
            if (ok) slot->function = known.function = fn;
            log("update rotation: %s UpdateRotation is vtable[%d] at %s (reads RotationInput +0x%X first): %s", cls.c_str(),
                index, where(fn).c_str(), unsigned(offset), ok ? "hooked, the gyro goes into RotationInput" : "hook failed");
        } else if (!known.function) {
            log("update rotation: %s: no free hook slot", cls.c_str());
        }
    }
    g_known.push_back(known);
    return known.function;
}

}  // namespace

bool is_input_property(const std::string& property) { return property == "RotationInput"; }

bool rotation_input(ue::Obj pc, int32_t& offset, int32_t& size) {
    static ue::Obj logged_class = 0;
    const ue::Obj cls = ue::class_of(pc);
    const ue::Prop reflected = ue::prop_of(pc, "RotationInput");
    const ue::Prop control = ue::prop_of(pc, "ControlRotation");
    ue::Prop scale = ue::prop_of(pc, "InputYawScale");
    if (!scale) scale = ue::prop_of(pc, "InputYawScale_DEPRECATED");
    const char* how = "reflected";
    if (reflected) {
        offset = reflected.offset, size = reflected.element_size;
    } else if (scale && control && (control.element_size == 12 || control.element_size == 24) &&
               scale.offset >= control.element_size) {
        offset = scale.offset - control.element_size, size = control.element_size;
        how = "before InputYawScale";
    } else {
        offset = -1, size = 0;
    }
    if (cls != logged_class) {
        logged_class = cls;
        if (offset >= 0)
            log("update rotation: %s RotationInput at +0x%X (%s, %d-byte rotator)", ue::name_of(cls).c_str(),
                unsigned(offset), how, int(size));
        else
            log("update rotation: %s has no RotationInput (not reflected, no InputYawScale): the gyro can't use it",
                ue::name_of(cls).c_str());
    }
    return offset >= 0;
}

bool add(ue::Obj pc, int32_t rotation_input_offset, int32_t rotator_size, double pitch, double yaw) {
    if (!pc || rotation_input_offset < 0 || !hooked(pc, rotation_input_offset, rotator_size)) return false;
    const ULONGLONG now = GetTickCount64();
    if (g_pending.pc != pc || (g_pending.since && now - g_pending.since > kStaleMs)) g_pending = {};
    g_pending.pc = pc;
    g_pending.offset = rotation_input_offset;
    g_pending.size = rotator_size;
    g_pending.pitch += pitch;
    g_pending.yaw += yaw;
    if (!g_pending.since) g_pending.since = now;
    return true;
}

void pending(ue::Obj pc, double& pitch, double& yaw) {
    const bool mine = pc && pc == g_pending.pc;
    pitch = mine ? g_pending.pitch : 0;
    yaw = mine ? g_pending.yaw : 0;
}

}  // namespace gyro4all::unreal::update_rotation
