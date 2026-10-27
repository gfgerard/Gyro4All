#pragma once
// COM vtable slot patching, shared by the DXGI hooks (overlay.cpp) and the D3D9 hooks (panel_host.cpp).
//
// The original function of every vtable slot we ever patched, by slot address. Each slot is patched ONCE: if another
// hooker overwrites it later, it chains to us, and patching it again would make us call that hooker's function as our
// "original" while it calls us back: an endless loop (stack overflow in Cronos with SCSKiller's d3d12.dll proxy, 0.6.1).
// Every hook calls the original of the slot its object came through.
#include <windows.h>

#include <mutex>
#include <unordered_map>

namespace gyro4all::com {

inline std::mutex& slot_mutex() {
    static std::mutex m;
    return m;
}

inline std::unordered_map<void**, void*>& slot_originals() {
    static std::unordered_map<void**, void*> originals;
    return originals;
}

inline bool patch_slot(void** vtable, int index, void* replacement, void** original) {
    void** const slot = &vtable[index];
    std::lock_guard lock(slot_mutex());
    if (slot_originals().count(slot) || *slot == replacement) return true;  // ours already (shared vtable)
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    slot_originals().emplace(slot, *slot);
    if (!*original) *original = *slot;  // fallback for objects whose vtable we never saw
    *slot = replacement;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

inline bool write_slot(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// Takes our hook out of a patched slot for a while: the slot holds its saved original again. True if it held `hook`.
inline bool unhook_slot(void** slot, void* hook) {
    std::lock_guard lock(slot_mutex());
    const auto it = slot_originals().find(slot);
    return it != slot_originals().end() && *slot == hook && write_slot(slot, it->second);
}

// Puts `hook` back into a slot unhook_slot emptied. A value another hooker wrote there meanwhile becomes the slot's
// original (it chains to the one we had) and is returned; null otherwise.
inline void* rehook_slot(void** slot, void* hook) {
    std::lock_guard lock(slot_mutex());
    const auto it = slot_originals().find(slot);
    if (it == slot_originals().end()) return nullptr;
    void* const now = *slot;
    void* foreign = nullptr;
    if (now != it->second && now != hook) it->second = foreign = now;
    write_slot(slot, hook);
    return foreign;
}

inline void** vtable_of(const void* object) { return *static_cast<void** const*>(object); }

// The function our hook must call for this object: the original we saved for its vtable slot.
template <class T>
T original_of(void* object, int index, T fallback) {
    void** const slot = vtable_of(object) + index;
    std::lock_guard lock(slot_mutex());
    const auto it = slot_originals().find(slot);
    return it != slot_originals().end() ? reinterpret_cast<T>(it->second) : fallback;
}

}  // namespace gyro4all::com
