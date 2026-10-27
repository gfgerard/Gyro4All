// The settings panel in Vulkan games: what gyro4all.dll needs to draw into the game's swapchain, gathered from the
// game's own Vulkan calls, and its presents handed to gyro4all.dll (g4a_api::vk_present). Two paths reach Vulkan:
//
// - vulkan-1.dll, through import hooks in the exe (DOOM 2016 and Eternal import every function they use);
// - NVIDIA Streamline's sl.interposer.dll (DOOM: The Dark Ages: after the first frames its presents only go through
//   the interposer, and its surface is made there), through inline hooks on the interposer's Vulkan exports as soon as
//   it loads (loader notification, as the DXGI path does for Streamline's factory in overlay.cpp). Inline hooks also
//   see calls through pointers the game fetched with GetProcAddress or the interposer's vkGet*ProcAddr (its exports).
//
// Each hook calls the original of its own path, so a swapchain made through the interposer has its images fetched
// through the interposer too (with frame generation they may not be the driver's). A call into one path from inside
// another hooked call on the same thread (the interposer calling its own exports, or vulkan-1's inline-hooked exports
// in a protected exe) is nested: only the outermost call is handled, so the panel is drawn once per present.
//
// - vkCreateInstance: the instance and the API version it was made for; vkCreateDevice: device -> physical device;
//   vkGetDeviceQueue/2: queue -> (device, family); vkCreateWin32SurfaceKHR: surface -> window.
// - vkCreateSwapchainKHR: the panel's swapchain (the latest one made with color-attachment usage), its images, format,
//   extent and window, which becomes the panel's window (panel_host::set_vk_target). vkGetSwapchainImagesKHR: the
//   images the game gets for it, if they differ from the ones read at creation.
// - vkQueuePresentKHR: for that swapchain, gyro4all.dll records and submits the panel's draw, waiting on the game's
//   semaphores, and the present then waits on gyro4all.dll's semaphore instead.
// - vkGetInstanceProcAddr / vkGetDeviceProcAddr: return these hooks for the same names, for games that fetch them.
#include "panel_vk.hpp"

#include "hooks.hpp"
#include "log.hpp"
#include "panel_host.hpp"

#include <gyro4all_panel.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gyro4all::panel_vk {
namespace {

enum Path { kLoader, kInterposer, kPaths };
const char* const kPathName[kPaths] = {"vulkan-1.dll", "sl.interposer.dll"};

// The originals of each path's hooks: vulkan-1.dll's imports (null until resolved: then its export), the interposer's
// MinHook trampolines (null if that export wasn't hooked).
struct Originals {
    PFN_vkCreateInstance create_instance = nullptr;
    PFN_vkCreateDevice create_device = nullptr;
    PFN_vkDestroyDevice destroy_device = nullptr;
    PFN_vkGetDeviceQueue get_device_queue = nullptr;
    PFN_vkGetDeviceQueue2 get_device_queue2 = nullptr;
    PFN_vkCreateWin32SurfaceKHR create_surface = nullptr;
    PFN_vkCreateSwapchainKHR create_swapchain = nullptr;
    PFN_vkDestroySwapchainKHR destroy_swapchain = nullptr;
    PFN_vkGetSwapchainImagesKHR get_swapchain_images = nullptr;
    PFN_vkQueuePresentKHR queue_present = nullptr;
    PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
    PFN_vkGetDeviceProcAddr get_device_proc = nullptr;
};
Originals g_orig[kPaths];
std::atomic<bool> g_sl_hooked{false};

struct Swapchain {
    VkDevice device = VK_NULL_HANDLE;
    std::vector<uint64_t> images;
    uint32_t format = 0, width = 0, height = 0;
    int path = kLoader;  // the one that made it
};

std::mutex g_mutex;  // the maps below; creation and destruction calls come from any game thread
VkInstance g_instance = VK_NULL_HANDLE;
uint32_t g_api_version = VK_API_VERSION_1_0;
std::unordered_map<VkDevice, VkPhysicalDevice> g_devices;
std::unordered_map<VkQueue, std::pair<VkDevice, uint32_t>> g_queues;
std::unordered_map<uint64_t, HWND> g_surfaces;
VkSwapchainKHR g_target = VK_NULL_HANDLE;  // the panel's swapchain
Swapchain g_target_info;

// Hooked calls this thread is inside of: a hook entered with one already running is nested.
thread_local int t_depth = 0;
struct Call {
    const bool nested = t_depth++ > 0;
    ~Call() { --t_depth; }
};

HMODULE loader() { return GetModuleHandleW(L"vulkan-1.dll"); }

template <class T>
T loader_proc(const char* name) {
    HMODULE m = loader();
    return m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
}

// The original of a path's hooked function; on vulkan-1.dll's path the loader's export when the import had none yet (a
// delay-load resolved later, or a function the exe only fetches through vkGet*ProcAddr).
template <int P, class T>
T real(T Originals::*field, const char* name) {
    if (T fn = g_orig[P].*field) return fn;
    return P == kLoader ? loader_proc<T>(name) : nullptr;
}

// The first few calls of a kind through each path are logged (which path the game uses, and when it changes).
bool first(std::atomic<int>* counters, int path, int limit = 1) { return counters[path].fetch_add(1) < limit; }

template <int P>
VkResult VKAPI_CALL hk_create_instance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* alloc,
                                       VkInstance* instance) {
    const auto fn = real<P>(&Originals::create_instance, "vkCreateInstance");
    const VkResult r = fn ? fn(info, alloc, instance) : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS && instance) {
        std::lock_guard lock(g_mutex);
        g_instance = *instance;
        g_api_version = info && info->pApplicationInfo && info->pApplicationInfo->apiVersion
                            ? info->pApplicationInfo->apiVersion
                            : VK_API_VERSION_1_0;
        log("panel: Vulkan instance %p (API %u.%u) through %s", static_cast<void*>(g_instance),
            VK_API_VERSION_MAJOR(g_api_version), VK_API_VERSION_MINOR(g_api_version), kPathName[P]);
    }
    return r;
}

template <int P>
VkResult VKAPI_CALL hk_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                     const VkAllocationCallbacks* alloc, VkDevice* device) {
    const auto fn = real<P>(&Originals::create_device, "vkCreateDevice");
    const VkResult r = fn ? fn(physical, info, alloc, device) : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS && device) {
        std::lock_guard lock(g_mutex);
        g_devices[*device] = physical;
        log("panel: Vulkan device %p through %s", static_cast<void*>(*device), kPathName[P]);
    }
    return r;
}

template <int P>
void VKAPI_CALL hk_destroy_device(VkDevice device, const VkAllocationCallbacks* alloc) {
    const Call call;
    if (!call.nested) {
        if (const g4a_api* a = panel_host::api(); a && device) a->vk_device_destroyed(device);
        std::lock_guard lock(g_mutex);
        g_devices.erase(device);
        for (auto it = g_queues.begin(); it != g_queues.end();)
            it = it->second.first == device ? g_queues.erase(it) : std::next(it);
        if (g_target_info.device == device) {
            g_target = VK_NULL_HANDLE;
            g_target_info = {};
        }
    }
    if (const auto fn = real<P>(&Originals::destroy_device, "vkDestroyDevice")) fn(device, alloc);
}

void add_queue(VkDevice device, uint32_t family, VkQueue queue) {
    if (!queue) return;
    std::lock_guard lock(g_mutex);
    g_queues[queue] = {device, family};
}

template <int P>
void VKAPI_CALL hk_get_device_queue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    if (const auto fn = real<P>(&Originals::get_device_queue, "vkGetDeviceQueue")) fn(device, family, index, queue);
    if (queue) add_queue(device, family, *queue);
}

template <int P>
void VKAPI_CALL hk_get_device_queue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    if (const auto fn = real<P>(&Originals::get_device_queue2, "vkGetDeviceQueue2")) fn(device, info, queue);
    if (queue && info) add_queue(device, info->queueFamilyIndex, *queue);
}

template <int P>
VkResult VKAPI_CALL hk_create_surface(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* info,
                                      const VkAllocationCallbacks* alloc, VkSurfaceKHR* surface) {
    const auto fn = real<P>(&Originals::create_surface, "vkCreateWin32SurfaceKHR");
    const VkResult r = fn ? fn(instance, info, alloc, surface) : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS && surface && info) {
        std::lock_guard lock(g_mutex);
        g_surfaces[uint64_t(*surface)] = info->hwnd;
    }
    return r;
}

// This process's largest visible, unowned top-level window (the game's), or null.
HWND main_window() {
    struct Best {
        HWND hwnd = nullptr;
        LONG area = 0;
    } best;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            RECT rc{};
            if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) ||
                !GetClientRect(hwnd, &rc))
                return TRUE;
            auto* b = reinterpret_cast<Best*>(param);
            const LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
            if (area > b->area) *b = {hwnd, area};
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&best));
    return best.hwnd;
}

template <int P>
VkResult VKAPI_CALL hk_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                        const VkAllocationCallbacks* alloc, VkSwapchainKHR* swapchain) {
    const Call call;
    const auto fn = real<P>(&Originals::create_swapchain, "vkCreateSwapchainKHR");
    const VkResult r = fn ? fn(device, info, alloc, swapchain) : VK_ERROR_INITIALIZATION_FAILED;
    if (call.nested || r != VK_SUCCESS || !swapchain || !info) return r;
    if (!(info->imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        log("panel: Vulkan swapchain %llx (through %s) has no color-attachment usage: the panel can't draw on it",
            static_cast<unsigned long long>(uint64_t(*swapchain)), kPathName[P]);
        return r;
    }
    Swapchain sc;
    sc.device = device;
    sc.format = uint32_t(info->imageFormat);
    sc.width = info->imageExtent.width;
    sc.height = info->imageExtent.height;
    sc.path = P;
    // Through the path that made it: the interposer may hand the game other images than the driver's.
    if (const auto get_images = real<P>(&Originals::get_swapchain_images, "vkGetSwapchainImagesKHR")) {
        uint32_t n = 0;
        if (get_images(device, *swapchain, &n, nullptr) == VK_SUCCESS && n) {
            std::vector<VkImage> images(n);
            if (get_images(device, *swapchain, &n, images.data()) >= VK_SUCCESS)
                for (uint32_t i = 0; i < n; ++i) sc.images.push_back(uint64_t(images[i]));
        }
    }
    HWND window = nullptr;
    {
        std::lock_guard lock(g_mutex);
        const auto it = g_surfaces.find(uint64_t(info->surface));
        window = it == g_surfaces.end() ? nullptr : it->second;
        g_target = *swapchain;
        g_target_info = sc;
    }
    // The surface was made where these hooks don't see it: the process's largest visible top-level window instead.
    const bool fallback = !window;
    if (!window) window = main_window();
    log("panel: Vulkan swapchain %llx through %s: %zu images %ux%u format %u, window %p%s",
        static_cast<unsigned long long>(uint64_t(*swapchain)), kPathName[P], sc.images.size(), sc.width, sc.height,
        sc.format, static_cast<void*>(window), fallback ? " (the process's main window: its surface wasn't seen)" : "");
    if (window && !sc.images.empty()) panel_host::set_vk_target(uint64_t(*swapchain), window);
    return r;
}

template <int P>
void VKAPI_CALL hk_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* alloc) {
    const Call call;
    if (!call.nested) {
        if (const g4a_api* a = panel_host::api(); a && swapchain) a->vk_swapchain_destroyed(uint64_t(swapchain));
        std::lock_guard lock(g_mutex);
        if (swapchain == g_target) {
            g_target = VK_NULL_HANDLE;
            g_target_info = {};
        }
    }
    if (const auto fn = real<P>(&Originals::destroy_swapchain, "vkDestroySwapchainKHR")) fn(device, swapchain, alloc);
}

template <int P>
VkResult VKAPI_CALL hk_get_swapchain_images(VkDevice device, VkSwapchainKHR swapchain, uint32_t* count, VkImage* images) {
    const Call call;
    const auto fn = real<P>(&Originals::get_swapchain_images, "vkGetSwapchainImagesKHR");
    const VkResult r = fn ? fn(device, swapchain, count, images) : VK_ERROR_INITIALIZATION_FAILED;
    // VK_SUCCESS with an array: *count is every image of the swapchain.
    if (call.nested || r != VK_SUCCESS || !images || !count || !*count) return r;
    std::vector<uint64_t> got;
    for (uint32_t i = 0; i < *count; ++i) got.push_back(uint64_t(images[i]));
    std::lock_guard lock(g_mutex);
    if (swapchain != g_target || got == g_target_info.images) return r;
    log("panel: the game gets other images for Vulkan swapchain %llx through %s (%u, %zu at creation): the panel uses "
        "the game's",
        static_cast<unsigned long long>(uint64_t(swapchain)), kPathName[P], *count, g_target_info.images.size());
    g_target_info.images = std::move(got);
    return r;
}

std::atomic<int> g_presents[kPaths], g_other_presents[kPaths];

template <int P>
VkResult VKAPI_CALL hk_queue_present(VkQueue queue, const VkPresentInfoKHR* info) {
    const Call call;
    const auto fn = real<P>(&Originals::queue_present, "vkQueuePresentKHR");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    const g4a_api* a = panel_host::api();
    if (call.nested || !a || !info) return fn(queue, info);
    g4a_vk_present p{};
    std::vector<uint64_t> waits, images;
    {
        std::lock_guard lock(g_mutex);
        uint32_t slot = UINT32_MAX;
        for (uint32_t i = 0; i < info->swapchainCount && g_target; ++i)
            if (info->pSwapchains[i] == g_target) slot = i;
        if (slot == UINT32_MAX) {
            if (info->swapchainCount && first(g_other_presents, P, 3))
                log("panel: a Vulkan present through %s on swapchain %llx, not the panel's (%llx)", kPathName[P],
                    static_cast<unsigned long long>(uint64_t(info->pSwapchains[0])),
                    static_cast<unsigned long long>(uint64_t(g_target)));
            return fn(queue, info);
        }
        const auto q = g_queues.find(queue);
        const auto d = g_devices.find(g_target_info.device);
        if (q == g_queues.end() || q->second.first != g_target_info.device || d == g_devices.end()) {
            if (first(g_other_presents, P, 3))
                log("panel: a Vulkan present through %s on the panel's swapchain from a queue %s", kPathName[P],
                    q == g_queues.end() ? "never seen" : "of another device");
            return fn(queue, info);
        }
        if (first(g_presents, P))
            log("panel: Vulkan presents of swapchain %llx (made through %s) come through %s",
                static_cast<unsigned long long>(uint64_t(g_target)), kPathName[g_target_info.path], kPathName[P]);
        p.size = sizeof p;
        p.api_version = g_api_version;
        // ImGui's functions come from the loader's vkGetInstanceProcAddr, beneath any interposer.
        p.get_instance_proc_addr = reinterpret_cast<void*>(real<kLoader>(&Originals::get_instance_proc, "vkGetInstanceProcAddr"));
        p.instance = g_instance;
        p.physical_device = d->second;
        p.device = g_target_info.device;
        p.queue = queue;
        p.queue_family = q->second.second;
        p.image_index = info->pImageIndices[slot];
        p.swapchain = uint64_t(g_target);
        images = g_target_info.images;  // a copy: the swapchain may be destroyed on another thread meanwhile
        p.format = g_target_info.format;
        p.width = g_target_info.width;
        p.height = g_target_info.height;
    }
    for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i) waits.push_back(uint64_t(info->pWaitSemaphores[i]));
    p.image_count = uint32_t(images.size());
    p.images = images.data();
    p.wait_semaphore_count = uint32_t(waits.size());
    p.wait_semaphores = waits.data();
    if (a->vk_present(&p) != G4A_OK || !p.signal_semaphore) return fn(queue, info);
    VkPresentInfoKHR changed = *info;
    const VkSemaphore panel = VkSemaphore(p.signal_semaphore);
    changed.waitSemaphoreCount = 1;
    changed.pWaitSemaphores = &panel;
    return fn(queue, &changed);
}

template <int P>
PFN_vkVoidFunction VKAPI_CALL hk_get_instance_proc(VkInstance instance, const char* name);
template <int P>
PFN_vkVoidFunction VKAPI_CALL hk_get_device_proc(VkDevice device, const char* name);

template <int P>
PFN_vkVoidFunction hook_for(const char* name) {
    if (!name) return nullptr;
    static const struct {
        const char* name;
        PFN_vkVoidFunction fn;
    } kHooks[] = {
        {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(&hk_create_instance<P>)},
        {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(&hk_create_device<P>)},
        {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(&hk_destroy_device<P>)},
        {"vkGetDeviceQueue", reinterpret_cast<PFN_vkVoidFunction>(&hk_get_device_queue<P>)},
        {"vkGetDeviceQueue2", reinterpret_cast<PFN_vkVoidFunction>(&hk_get_device_queue2<P>)},
        {"vkCreateWin32SurfaceKHR", reinterpret_cast<PFN_vkVoidFunction>(&hk_create_surface<P>)},
        {"vkCreateSwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(&hk_create_swapchain<P>)},
        {"vkDestroySwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(&hk_destroy_swapchain<P>)},
        {"vkGetSwapchainImagesKHR", reinterpret_cast<PFN_vkVoidFunction>(&hk_get_swapchain_images<P>)},
        {"vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(&hk_queue_present<P>)},
        {"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(&hk_get_instance_proc<P>)},
        {"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(&hk_get_device_proc<P>)},
    };
    for (const auto& h : kHooks)
        if (!std::strcmp(h.name, name)) return h.fn;
    return nullptr;
}

// The hook to hand out for a function the path's vkGet*ProcAddr returned (fn), or fn itself. vulkan-1.dll's: the hook
// for any known name. The interposer's: fn itself, which already reaches the hook when it is the interposer's export of
// that name (hooked inline); a function of its own under that name would bypass the hooks (logged).
template <int P>
PFN_vkVoidFunction resolved(const char* name, PFN_vkVoidFunction fn) {
    if (!fn) return fn;
    const PFN_vkVoidFunction h = hook_for<P>(name);
    if (P == kLoader) return h ? h : fn;
    if (h && reinterpret_cast<FARPROC>(fn) != GetProcAddress(GetModuleHandleW(L"sl.interposer.dll"), name)) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 8)
            log("panel: sl.interposer.dll returns its own %s, not its export: not seen by the panel", name);
    }
    return fn;
}

template <int P>
PFN_vkVoidFunction VKAPI_CALL hk_get_instance_proc(VkInstance instance, const char* name) {
    const auto fn = real<P>(&Originals::get_instance_proc, "vkGetInstanceProcAddr");
    return resolved<P>(name, fn ? fn(instance, name) : nullptr);
}

template <int P>
PFN_vkVoidFunction VKAPI_CALL hk_get_device_proc(VkDevice device, const char* name) {
    const auto fn = real<P>(&Originals::get_device_proc, "vkGetDeviceProcAddr");
    return resolved<P>(name, fn ? fn(device, name) : nullptr);
}

// Each hook with its path's original, by name.
struct Hook {
    const char* name;
    void* replacement;
    void** original;
};

template <int P, class T, class H>
Hook hook(const char* name, H* replacement, T Originals::*field) {
    return {name, reinterpret_cast<void*>(replacement), reinterpret_cast<void**>(&(g_orig[P].*field))};
}

template <int P>
std::vector<Hook> hooks_of() {
    return {
        hook<P>("vkCreateInstance", &hk_create_instance<P>, &Originals::create_instance),
        hook<P>("vkCreateDevice", &hk_create_device<P>, &Originals::create_device),
        hook<P>("vkDestroyDevice", &hk_destroy_device<P>, &Originals::destroy_device),
        hook<P>("vkGetDeviceQueue", &hk_get_device_queue<P>, &Originals::get_device_queue),
        hook<P>("vkGetDeviceQueue2", &hk_get_device_queue2<P>, &Originals::get_device_queue2),
        hook<P>("vkCreateWin32SurfaceKHR", &hk_create_surface<P>, &Originals::create_surface),
        hook<P>("vkCreateSwapchainKHR", &hk_create_swapchain<P>, &Originals::create_swapchain),
        hook<P>("vkDestroySwapchainKHR", &hk_destroy_swapchain<P>, &Originals::destroy_swapchain),
        hook<P>("vkGetSwapchainImagesKHR", &hk_get_swapchain_images<P>, &Originals::get_swapchain_images),
        hook<P>("vkQueuePresentKHR", &hk_queue_present<P>, &Originals::queue_present),
        hook<P>("vkGetInstanceProcAddr", &hk_get_instance_proc<P>, &Originals::get_instance_proc),
        hook<P>("vkGetDeviceProcAddr", &hk_get_device_proc<P>, &Originals::get_device_proc),
    };
}

// Loader notification (inside the loader lock) or install(): before the game resolves any of these exports.
void hook_interposer(HMODULE interposer) {
    if (g_sl_hooked.exchange(true)) return;
    std::string result;
    int ok_count = 0;
    for (const Hook& h : hooks_of<kInterposer>()) {
        void* target = reinterpret_cast<void*>(GetProcAddress(interposer, h.name));
        const bool ok = target && hooks::hook_function(target, h.replacement, h.original);
        if (ok) ++ok_count;
        else result += std::string(result.empty() ? "" : ", ") + h.name + (target ? " failed" : " missing");
    }
    log("panel: sl.interposer.dll loaded at %p: %d Vulkan exports hooked%s%s", static_cast<void*>(interposer), ok_count,
        result.empty() ? "" : "; ", result.c_str());
}

}  // namespace

void install() {
    for (const Hook& h : hooks_of<kLoader>()) hooks::add("vulkan-1.dll", h.name, h.replacement, h.original);
    if (!hooks::watch_dll(L"sl.interposer.dll", &hook_interposer))
        log("panel: cannot watch DLL loads: Vulkan through Streamline's interposer not available to the panel");
}

}  // namespace gyro4all::panel_vk
