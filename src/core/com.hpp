#pragma once
// COM vtable slot patching, shared by the DXGI hooks (overlay.cpp) and the D3D9 hooks (panel_host.cpp).
//
// The original function of every vtable slot we ever patched, by slot address. Each slot is patched ONCE: if another
// hooker overwrites it later, it chains to us, and patching it again would make us call that hooker's function as our
// "original" while it calls us back: an endless loop (stack overflow in Cronos with SCSKiller's d3d12.dll proxy, 0.6.1).
// Every hook calls the original of the slot its object came through.
#include <windows.h>

#include <algorithm>
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

// Our hook in every vtable slot we patched, by slot address.
inline std::unordered_map<void**, void*>& slot_hooks() {
    static std::unordered_map<void**, void*> hooks;
    return hooks;
}

inline bool patch_slot(void** vtable, int index, void* replacement, void** original) {
    void** const slot = &vtable[index];
    std::lock_guard lock(slot_mutex());
    if (slot_originals().count(slot) || *slot == replacement) return true;  // ours already (shared vtable)
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    slot_originals().emplace(slot, *slot);
    slot_hooks().emplace(slot, replacement);
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

// Private vtables: copies of a shared vtable with our hooks in them, given to single objects (make_private), by copy ->
// the shared vtable it was copied from. The shared vtable is never patched, so another hooker that reads it (to hook it,
// or to save "the original" of a slot) never sees our hooks and never calls them as its original: Steam's overlay reads
// the Present slot of every new swapchain, also of ones made where our factory hooks don't see them (REFramework's dummy
// swapchain in RE4, MODLOG 2026-11-04), and a shared-vtable hook of ours seen there made it and us call each other.
inline std::unordered_map<void**, void**>& private_vtables() {
    static std::unordered_map<void**, void**> copies;
    return copies;
}

// The objects we gave a private vtable, by object -> the shared vtable. Another hooker may later point the object to its
// own copy of our copy (RE4, MODLOG 2026-11-04: a SetColorSpace1 hook called a null original); the object still tells
// which shared vtable our hooks call through.
inline std::unordered_map<void*, void**>& private_objects() {
    static std::unordered_map<void*, void**> objects;
    return objects;
}

inline bool is_private_vtable(void** vtable) {
    std::lock_guard lock(slot_mutex());
    return private_vtables().count(vtable) != 0;
}

// The function our hook must call for this object: for an object with a private vtable (ours, or another hooker's copy of
// ours), what the shared vtable's slot holds now (another hooker's hook of the slot runs too), or the original we saved if
// that slot holds one of our hooks as well; on a patched shared vtable, the original we saved for its slot.
template <class T>
T original_of(void* object, int index, T fallback) {
    void** const vtable = vtable_of(object);
    std::lock_guard lock(slot_mutex());
    void** shared = nullptr;
    if (const auto p = private_vtables().find(vtable); p != private_vtables().end())
        shared = p->second;
    else if (const auto o = private_objects().find(object); o != private_objects().end())
        shared = o->second;
    if (shared) {
        void** const slot = shared + index;
        const auto it = slot_originals().find(slot);
        const auto hook = slot_hooks().find(slot);
        const bool ours = it != slot_originals().end() && hook != slot_hooks().end() && *slot == hook->second;
        return reinterpret_cast<T>(ours ? it->second : *slot);
    }
    const auto it = slot_originals().find(vtable + index);
    return it != slot_originals().end() ? reinterpret_cast<T>(it->second) : fallback;
}

inline bool is_code(const void* address) {
    MEMORY_BASIC_INFORMATION m{};
    return address && VirtualQuery(address, &m, sizeof(m)) == sizeof(m) && m.State == MEM_COMMIT &&
           (m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

struct SlotHook {
    int index;
    void* hook;
};

// Gives `object` a private copy of its vtable with `hooks` in it (one copy per shared vtable, kept for the process's life:
// objects may still point to it after we let go of them). The copy runs to the end of the shared vtable's run of code
// pointers, so the class's own virtuals after the interface's are kept, with the entry before it (RTTI). False, and
// nothing changed, if that run is shorter than the hooks need.
inline bool make_private(void* object, const SlotHook* hooks, size_t count) {
    void** const shared = vtable_of(object);
    std::lock_guard lock(slot_mutex());
    if (private_vtables().count(shared)) return true;  // ours already
    void** copy = nullptr;
    for (const auto& [c, s] : private_vtables())
        if (s == shared) copy = c;
    if (!copy) {
        MEMORY_BASIC_INFORMATION m{};
        if (VirtualQuery(shared, &m, sizeof(m)) != sizeof(m) || m.State != MEM_COMMIT) return false;
        const auto begin = static_cast<void**>(m.BaseAddress);
        void** const end = reinterpret_cast<void**>(static_cast<char*>(m.BaseAddress) + m.RegionSize);
        size_t n = 0;
        while (n < 1024 && shared + n < end && is_code(shared[n])) ++n;
        int needed = 0;
        for (size_t i = 0; i < count; ++i) needed = (std::max)(needed, hooks[i].index + 1);
        if (n < size_t(needed)) return false;
        void** const block = new void*[n + 1];
        block[0] = shared > begin ? shared[-1] : nullptr;
        for (size_t i = 0; i < n; ++i) block[i + 1] = shared[i];
        copy = block + 1;
        for (size_t i = 0; i < count; ++i) copy[hooks[i].index] = hooks[i].hook;
        private_vtables().emplace(copy, shared);
    }
    private_objects()[object] = shared;
    InterlockedExchangePointer(static_cast<void**>(object), copy);
    return true;
}

}  // namespace gyro4all::com
