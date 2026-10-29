#pragma once
// One COM vtable slot patched in place, in each vtable seen (up to 4: e.g. dinput8.dll has one per A/W flavour). The
// vtables are shared by every object of their class, also those of other modules (GyroLib's SDL): the hooks check what
// they were called for.
#include <windows.h>

#include <atomic>
#include <mutex>

namespace gyro4all {

struct VtableHook {
    std::mutex mutex;
    void** vtables[4] = {};
    void* originals[4] = {};
    std::atomic<int> count{0};

    void* original_for(const void* object) {
        void** vt = object ? *static_cast<void** const*>(object) : nullptr;
        for (int i = 0, n = count.load(std::memory_order_acquire); i < n; ++i)
            if (vtables[i] == vt) return originals[i];
        return nullptr;
    }

    void patch(void* object, int index, void* replacement) {
        void** vt = *static_cast<void***>(object);
        std::lock_guard lock(mutex);
        const int n = count.load();
        for (int i = 0; i < n; ++i)
            if (vtables[i] == vt) return;
        DWORD old = 0;
        if (n == 4 || vt[index] == replacement ||
            !VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
            return;
        vtables[n] = vt;
        originals[n] = vt[index];
        count.store(n + 1, std::memory_order_release);  // before the patch: a call may arrive right after it
        InterlockedExchangePointer(&vt[index], replacement);
        VirtualProtect(&vt[index], sizeof(void*), old, &old);
    }
};

}  // namespace gyro4all
