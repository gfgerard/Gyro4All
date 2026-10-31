#include "overlay.hpp"

#include "com.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "panel_host.hpp"
#include "perf.hpp"
#include "settings.hpp"

#include <intrin.h>
#include <windows.h>

#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <gyro4all_panel.h>
#include <gyrolib/gyrolib.h>
#include <gyrolib/overlay.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace gyro4all::overlay {
namespace {

// ---------------------------------------------------------------- hook plumbing (com.hpp)

bool patch_vtable(void** vtable, int index, void* replacement, void** original) {
    return com::patch_slot(vtable, index, replacement, original);
}

template <class T>
T original_of(void* object, int index, T fallback) {
    return com::original_of(object, index, fallback);
}

// ---------------------------------------------------------------- state

using CreateFactory_t = HRESULT(WINAPI*)(REFIID, void**);
using CreateSwapChain_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwnd_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
                                                     IUnknown* const*);

using CreateFactory2_t = HRESULT(WINAPI*)(UINT, REFIID, void**);
CreateFactory_t g_create_factory = nullptr, g_create_factory1 = nullptr;
CreateFactory2_t g_create_factory2 = nullptr;
CreateSwapChain_t g_create_swapchain = nullptr;
CreateSwapChainForHwnd_t g_create_swapchain_hwnd = nullptr;
Present_t g_present = nullptr;
Present1_t g_present1 = nullptr;
ResizeBuffers_t g_resize = nullptr;
ResizeBuffers1_t g_resize1 = nullptr;
WNDPROC g_wndproc = nullptr;

std::mutex g_mutex;                       // guards swapchain/queue handoff
IDXGISwapChain3* g_swapchain = nullptr;   // captured game swapchain (AddRef'd), the one its Present is called on
ID3D12CommandQueue* g_queue = nullptr;    // its DIRECT queue (AddRef'd)
// What GyroLib renders with (AddRef'd): the same as above, unless wrappers make their devices differ (then a matching
// pair of their native objects, see on_swapchain).
IDXGISwapChain3* g_render_swapchain = nullptr;
ID3D12CommandQueue* g_render_queue = nullptr;
// References on g_swapchain that are ours or GyroLib's (on Streamline's factory: when its count falls to this, the
// game has let go of it, and so do we; see hk_release).
ULONG g_held_refs = 0;

ULONG refs_of(IUnknown* object) {
    object->AddRef();
    return object->Release();
}
HWND g_hwnd = nullptr;
std::atomic<gl_overlay*> g_overlay{nullptr};
// Next to gyro4all.dll's static panel: a gl_overlay that never renders and is never fed window messages. GyroLib shows
// its per-view "Steam Input mouse" row (input.steam_mouse) only while one of its mouse bridges exists; this one gives
// that row to the static panel. GyroLib's bridge confirms Steam's mouse from Raw Input messages it never gets here, so
// it never intercepts anything itself: steammouse.cpp applies the row's choice (MODLOG 2026-10-31).
gl_overlay* g_bridge = nullptr;
bool g_bridge_tried = false;
std::atomic<bool> g_renderer_ready{false}, g_renderer_failed{false}, g_reinit{false};
bool g_subclassed = false;
HWND g_subclassed_hwnd = nullptr;  // the window hk_wndproc is on
std::atomic<UINT> g_menu_vk{0};  // virtual key of the panel shortcut (handled by polling)
bool g_menu_key_down = false;
LARGE_INTEGER g_freq{}, g_last_present{};
UINT g_buffer_count = 0;
DXGI_FORMAT g_buffer_format = DXGI_FORMAT_UNKNOWN;
DWORD g_render_thread = 0;
std::atomic<bool> g_buffers_held{false};  // GyroLib holds backbuffer references
// Diagnostics: panel visibility and render results are logged on change.
int32_t g_last_render_result = GL_OK;
uint32_t g_last_capture = 0;
bool g_last_panel_open = false;
void* g_foreign_swapchain = nullptr;

// ---------------------------------------------------------------- render worker
//
// GyroLib binds its DX12 renderer to ONE thread (init/render/before_resize/shutdown), but UE may move
// Present between threads (Cronos: after a ResizeBuffers, Present moved from one thread to another and
// every render failed with "render requires initialized render thread"), and resize from yet another
// thread (Returnal). So every GyroLib render call runs on our own worker thread: the presenting/resizing
// thread hands the job over and waits, which keeps the order (panel drawn before the game's Present).
// GyroLib must see a render every frame even while the panel is closed: it refuses/closes the panel when the
// render side is older than 250 ms (0.4.4 skipped closed frames and F10 stopped working).

std::mutex g_job_mutex;  // one job at a time (Present and ResizeBuffers may come from different threads)
HANDLE g_job_event = nullptr, g_done_event = nullptr;
void (*g_job)() = nullptr;

DWORD WINAPI worker_main(void*) {
    for (;;) {
        WaitForSingleObject(g_job_event, INFINITE);
        g_job();
        SetEvent(g_done_event);
    }
}

// Runs job on the render worker and waits for it. False if the worker is unavailable or stuck.
bool on_worker(void (*job)()) {
    std::lock_guard lock(g_job_mutex);
    if (g_renderer_failed) return false;
    if (!g_job_event) {
        g_job_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_done_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE thread = g_job_event && g_done_event ? CreateThread(nullptr, 0, &worker_main, nullptr, 0, &g_render_thread) : nullptr;
        if (!thread) {
            log("overlay: cannot start the render worker (error %lu); panel disabled", GetLastError());
            g_renderer_failed = true;
            return false;
        }
        CloseHandle(thread);
        log("overlay: render worker thread %lu", g_render_thread);
    }
    g_job = job;
    SetEvent(g_job_event);
    if (WaitForSingleObject(g_done_event, 5000) != WAIT_OBJECT_0) {
        log("overlay: render worker stuck for 5 s; panel disabled");
        g_renderer_failed = true;
        return false;
    }
    return true;
}

// Worker jobs read the captured swapchain under g_mutex.
void render_job() {
    gl_overlay* overlay = g_overlay.load();
    if (!overlay || g_renderer_failed) return;
    IDXGISwapChain3* swapchain = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    HWND hwnd = nullptr;
    {
        std::lock_guard lock(g_mutex);
        swapchain = g_render_swapchain, queue = g_render_queue, hwnd = g_hwnd;
    }
    if (g_reinit.exchange(false) && g_renderer_ready) {
        const int32_t r = gl_overlay_dx12_shutdown(overlay);
        log("overlay: renderer shutdown for re-init (%d)%s%s", r, r != GL_OK ? ": " : "", r != GL_OK ? gl_overlay_error() : "");
        g_renderer_ready = false;
        g_buffers_held = false;
    }
    if (!g_renderer_ready) {
        const gl_overlay_dx12_desc desc{sizeof(desc), GL_OVERLAY_ABI_VERSION, GL_OVERLAY_COLOR_SPACE_AUTO, 0, hwnd, swapchain, queue};
        const ULONG before = refs_of(swapchain);
        const int32_t r = gl_overlay_dx12_init(overlay, &desc);
        const ULONG after = refs_of(swapchain);
        if (r != GL_OK) {
            log("overlay: DX12 init failed (%d): %s; panel disabled", r, gl_overlay_error());
            g_renderer_failed = true;
            return;
        }
        g_renderer_ready = true;
        {
            std::lock_guard lock(g_mutex);
            if (swapchain == g_swapchain && after > before) g_held_refs += after - before;
        }
        DXGI_SWAP_CHAIN_DESC1 d{};
        swapchain->GetDesc1(&d);
        g_buffer_count = d.BufferCount;
        g_buffer_format = d.Format;
        log("overlay: DX12 panel ready (render worker %lu, %u buffers, format %d; GyroLib holds %ld swapchain references)",
            g_render_thread, d.BufferCount, int(d.Format), long(after) - long(before));
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = g_last_present.QuadPart ? double(now.QuadPart - g_last_present.QuadPart) / double(g_freq.QuadPart) : 1.0 / 60;
    g_last_present = now;
    if (dt <= 0 || dt > 1) dt = 1.0 / 60;
    const UINT dpi = GetDpiForWindow(hwnd);
    const int32_t r = gl_overlay_dx12_render(overlay, dt, dpi ? dpi / 96.0f : 1.0f);
    if (r == GL_IO_ERROR) {
        log("overlay: render device error: %s; panel disabled", gl_overlay_error());
        g_renderer_failed = true;
        return;
    }
    if (r != g_last_render_result) {
        log("overlay: render returned %d%s%s", r, r != GL_OK ? ": " : "", r != GL_OK ? gl_overlay_error() : "");
        g_last_render_result = r;
    }
    const uint32_t cap = gl_overlay_capture(overlay);
    if ((cap != 0) != (g_last_capture != 0)) log("overlay: panel %s (capture %u)", cap ? "drawn" : "hidden", cap);
    g_last_capture = cap;
    g_buffers_held = true;
    // Closed: nothing is drawn, so release the backbuffer references now; a resize then never has to
    // wait for GyroLib.
    if (cap == 0 && gl_overlay_dx12_before_resize(overlay) == GL_OK) g_buffers_held = false;
}

void before_resize_job() {
    gl_overlay* overlay = g_overlay.load();
    if (!overlay || !g_renderer_ready || !g_buffers_held) return;
    const int32_t r = gl_overlay_dx12_before_resize(overlay);
    if (r == GL_OK) g_buffers_held = false;
    log("overlay: before resize (%d)%s%s", r, r != GL_OK ? ": " : "", r != GL_OK ? gl_overlay_error() : "");
}

bool is_captured(IDXGISwapChain* sc) {
    std::lock_guard lock(g_mutex);
    return static_cast<IUnknown*>(sc) == static_cast<IUnknown*>(g_swapchain);
}

// A Present nested in one of our Present hooks (a wrapper's Present calling the real one through a hooked vtable) draws
// nothing more: the non-DX12 panel is drawn once per frame.
thread_local int t_presenting = 0;
std::atomic<PresentFn> g_on_present{nullptr};

void note_present() {
    if (t_presenting) return;
    if (PresentFn fn = g_on_present.load(std::memory_order_relaxed)) fn();
}

bool adopt_presented(IDXGISwapChain* sc);

void render(IDXGISwapChain* sc) {
    if (panel_host::is_dxgi_target(sc) || adopt_presented(sc)) {  // D3D10/D3D11: gyro4all.dll draws the static panel
        if (!t_presenting) panel_host::dxgi_present(sc);
        return;
    }
    if (!g_overlay.load() || g_renderer_failed) return;
    if (!is_captured(sc)) {
        if (g_foreign_swapchain != sc) {
            g_foreign_swapchain = sc;
            log("overlay: Present on a swapchain that wasn't captured (%p); panel not drawn there", static_cast<void*>(sc));
        }
        return;
    }
    on_worker(&render_job);
}

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) {
        note_present();
        const int64_t start = perf::now();
        render(sc);
        perf::record(perf::Present, start);
    }
    ++t_presenting;
    const HRESULT hr = original_of(sc, 8, g_present)(sc, sync, flags);
    --t_presenting;
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    if (!(flags & DXGI_PRESENT_TEST)) {
        note_present();
        const int64_t start = perf::now();
        render(sc);
        perf::record(perf::Present, start);
    }
    ++t_presenting;
    const HRESULT hr = original_of(sc, 22, g_present1)(sc, sync, flags, params);
    --t_presenting;
    return hr;
}

// GyroLib's documented resize path: release backbuffer references before ResizeBuffers (on the worker,
// whatever thread resizes); the next render reacquires them. A changed count/format needs a re-init.
void before_resize(IDXGISwapChain* sc, UINT count, DXGI_FORMAT format) {
    panel_host::dxgi_before_resize(sc);  // D3D10/D3D11 target only (it holds no backbuffer, only size-dependent textures)
    if (!g_overlay.load() || !is_captured(sc) || !g_renderer_ready) return;
    if (g_buffers_held) on_worker(&before_resize_job);
    const bool changed = (count != 0 && count != g_buffer_count) || (format != DXGI_FORMAT_UNKNOWN && format != g_buffer_format);
    if (changed) {
        g_reinit = true;
        log("overlay: resize changes buffer count/format: re-init on next Present");
    }
}

// ReShade add-ons such as RenoDX can change the real buffer format behind the game's back (e.g. the game asks
// for RGB10A2 and gets RGBA16F). GyroLib needs a full re-init when count/format change, so compare against
// what the swapchain really has after the resize, not against the arguments.
void after_resize(IDXGISwapChain* sc, HRESULT hr, const char* what, UINT count, UINT w, UINT h, DXGI_FORMAT format) {
    DXGI_SWAP_CHAIN_DESC d{};
    const bool have = SUCCEEDED(hr) && SUCCEEDED(sc->GetDesc(&d));
    bool ours = false;
    {
        std::lock_guard lock(g_mutex);
        ours = static_cast<IUnknown*>(sc) == static_cast<IUnknown*>(g_swapchain);
    }
    const bool changed = have && ours && g_renderer_ready &&
                         (d.BufferCount != g_buffer_count || d.BufferDesc.Format != g_buffer_format);
    if (changed) g_reinit = true;
    log("overlay: %s(%u, %ux%u, format %d) on thread %lu -> 0x%08lX; real buffers %u, format %d%s", what, count, w, h,
        int(format), GetCurrentThreadId(), static_cast<unsigned long>(hr), have ? d.BufferCount : 0,
        have ? int(d.BufferDesc.Format) : -1, changed ? ": changed, panel re-init on next Present" : "");
}

HRESULT STDMETHODCALLTYPE hk_resize(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags) {
    before_resize(sc, count, format);
    const HRESULT hr = original_of(sc, 13, g_resize)(sc, count, w, h, format, flags);
    after_resize(sc, hr, "ResizeBuffers", count, w, h, format);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_resize1(IDXGISwapChain3* sc, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags,
                                     const UINT* masks, IUnknown* const* queues) {
    before_resize(sc, count, format);
    const HRESULT hr = original_of(sc, 39, g_resize1)(sc, count, w, h, format, flags, masks, queues);
    after_resize(sc, hr, "ResizeBuffers1", count, w, h, format);
    return hr;
}

// D3D10/D3D11 HDR: DXGI can't be asked which colour space a swapchain shows, so the panel remembers what the game sets.
using SetColorSpace1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
SetColorSpace1_t g_set_color_space1 = nullptr;

HRESULT STDMETHODCALLTYPE hk_set_color_space1(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE space) {
    const HRESULT hr = original_of(sc, 38, g_set_color_space1)(sc, space);
    if (SUCCEEDED(hr)) panel_host::set_color_space(sc, uint32_t(space));
    return hr;
}

// ---------------------------------------------------------------- swapchain capture

// On Streamline's factory, the panel must not keep the game's swapchain alive. Toggling frame generation in RE
// Requiem makes the game release its swapchain, unload frame generation and create a new swapchain for the same window.
// While the panel (Gyro4All and GyroLib) still held the old one, Streamline's proxy outlived frame generation: the new
// creation crashed in sl.dlss_g/sl.common, or (released just before it) failed with E_ACCESSDENIED forever. So the
// panel lets go when the game does (hk_release), and before a new swapchain for the window as a fallback.
using Release_t = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
Release_t g_release = nullptr;
bool g_release_with_game = false;
bool g_off_worker = false;  // release_job called without the worker: GyroLib's renderer is left alone
const char* g_release_reason = "";
thread_local bool t_releasing = false;
ULONG STDMETHODCALLTYPE hk_release(IUnknown* self);

std::string module_of(const void* address) {
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &m))
        GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    return narrow(name ? name + 1 : path);
}

std::string describe(const void* object) {
    char text[64];
    snprintf(text, sizeof(text), "%p", object);
    return object ? std::string(text) + " (" + module_of(*static_cast<void* const*>(object)) + ")" : "none";
}

// ---------------------------------------------------------------- swapchain slots during a creation
//
// Steam's overlay (gameoverlayrenderer64.dll, injected even with its overlay off) hooks the code of the function a new
// swapchain's Present slot points to, each time the game creates a swapchain, and keeps one saved original for it. With
// our hook in the slot, the second swapchain made it hook our hook: its original became our hook, also for its first
// hook on dxgi's Present, and the two called each other until the stack overflowed (Metro 2033 Redux, MODLOG
// 2026-10-23). So while the game creates a swapchain, the swapchain slots we patched hold their originals again and
// another hooker sees the same functions as at the first creation; our hooks go back in once it returns.

struct SwapchainSlot {
    void** slot;
    void* hook;
    bool unhooked = false;
};
std::mutex g_sc_slots_mutex;  // before com::slot_mutex when both are taken
std::vector<SwapchainSlot> g_sc_slots;
int g_sc_creating = 0;  // swapchain creations in progress (nested ones count once)

bool patch_swapchain_slot(void** vtable, int index, void* replacement, void** original) {
    std::lock_guard lock(g_sc_slots_mutex);
    if (!patch_vtable(vtable, index, replacement, original)) return false;
    void** const slot = &vtable[index];
    if (std::none_of(g_sc_slots.begin(), g_sc_slots.end(), [slot](const SwapchainSlot& s) { return s.slot == slot; }))
        g_sc_slots.push_back({slot, replacement});
    return true;
}

void unhook_swapchain_slots() {
    std::lock_guard lock(g_sc_slots_mutex);
    if (g_sc_creating++ > 0) return;
    int n = 0;
    for (SwapchainSlot& s : g_sc_slots) n += s.unhooked = com::unhook_slot(s.slot, s.hook);
    if (n) log("overlay: %d swapchain hook(s) set aside while the game creates a swapchain", n);
}

void rehook_swapchain_slots() {
    std::lock_guard lock(g_sc_slots_mutex);
    if (g_sc_creating == 0 || --g_sc_creating > 0) return;
    for (SwapchainSlot& s : g_sc_slots) {
        if (!s.unhooked) continue;
        s.unhooked = false;
        if (void* foreign = com::rehook_slot(s.slot, s.hook))
            log("overlay: a swapchain slot was hooked by %s during the creation; it is our original now",
                module_of(foreign).c_str());
    }
}

// Streamline's slGetNativeInterface: the object a Streamline proxy wraps; fails on anything
// that isn't one of its proxies. Whether it AddRefs what it returns is measured on the first call (two calls, reference
// counts compared), so the native objects are never over-released.
using SlGetNativeInterface_t = int(__cdecl*)(void*, void**);
SlGetNativeInterface_t g_sl_native = nullptr;
int g_sl_native_addrefs = -1;  // -1 = not measured yet

// The native object under a Streamline proxy, AddRef'd once (ours to release), or null.
template <class T>
T* native_of(T* proxy) {
    void* base = nullptr;
    if (!g_sl_native || !proxy || g_sl_native(proxy, &base) != 0 || !base) return nullptr;
    IUnknown* const unknown = static_cast<IUnknown*>(base);
    if (g_sl_native_addrefs < 0) {
        const ULONG before = refs_of(unknown);
        void* again = nullptr;
        g_sl_native(proxy, &again);
        const ULONG after = refs_of(unknown);
        g_sl_native_addrefs = after > before ? 1 : 0;
        if (again && g_sl_native_addrefs) unknown->Release();  // the second call's reference
        log("overlay: slGetNativeInterface %s the native object (references %lu -> %lu)",
            g_sl_native_addrefs ? "AddRefs" : "does not AddRef", before, after);
    }
    T* native = nullptr;
    unknown->QueryInterface(IID_PPV_ARGS(&native));
    if (g_sl_native_addrefs) unknown->Release();
    return native == proxy ? (native->Release(), nullptr) : native;
}

// The D3D12 device an object reports (not AddRef'd: only compared).
template <class T>
void* device_of(T* object) {
    ID3D12Device* device = nullptr;
    if (!object || FAILED(object->GetDevice(IID_PPV_ARGS(&device))) || !device) return nullptr;
    device->Release();
    return device;
}

void on_swapchain(IUnknown* device, HWND hwnd, IUnknown* created) {
    ID3D12CommandQueue* queue = nullptr;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) {
        log("overlay: swapchain is not D3D12; panel unavailable");
        return;
    }
    D3D12_COMMAND_QUEUE_DESC qd = queue->GetDesc();
    IDXGISwapChain3* sc3 = nullptr;
    if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT || FAILED(created->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        queue->Release();
        log("overlay: unsupported swapchain/queue; panel unavailable");
        return;
    }
    // GyroLib needs the queue and the swapchain on the same device. Wrappers (Streamline's proxies, ReShade) can make
    // them report different device objects: then try the native objects under Streamline's proxies.
    IDXGISwapChain3* render_sc = sc3;
    ID3D12CommandQueue* render_queue = queue;
    if (device_of(sc3) != device_of(queue)) {
        IDXGISwapChain3* native_sc = native_of(sc3);
        ID3D12CommandQueue* native_queue = native_of(queue);
        log("overlay: queue %s on device %s, swapchain %s on device %s; native swapchain %s on device %s, native queue %s "
            "on device %s",
            describe(queue).c_str(), describe(device_of(queue)).c_str(), describe(sc3).c_str(),
            describe(device_of(sc3)).c_str(), describe(native_sc).c_str(), describe(device_of(native_sc)).c_str(),
            describe(native_queue).c_str(), describe(device_of(native_queue)).c_str());
        IDXGISwapChain3* const scs[] = {sc3, native_sc};
        ID3D12CommandQueue* const queues[] = {queue, native_queue};
        bool found = false;
        for (IDXGISwapChain3* s : scs)
            for (ID3D12CommandQueue* q : queues)
                if (!found && s && q && device_of(s) && device_of(s) == device_of(q)) render_sc = s, render_queue = q, found = true;
        log("overlay: queue and swapchain devices differ: %s", !found ? "no matching pair, GyroLib will refuse it"
                                                              : render_sc == sc3 ? "rendering with the native queue"
                                                              : render_queue == queue ? "rendering on the native swapchain"
                                                                                      : "rendering on the native swapchain and queue");
        if (render_sc == sc3 && native_sc) native_sc->Release();
        if (render_queue == queue && native_queue) native_queue->Release();
    }
    if (render_sc == sc3) sc3->AddRef();
    if (render_queue == queue) queue->AddRef();
    void** vtable = *reinterpret_cast<void***>(sc3);
    patch_swapchain_slot(vtable, 8, reinterpret_cast<void*>(&hk_present), reinterpret_cast<void**>(&g_present));
    patch_swapchain_slot(vtable, 13, reinterpret_cast<void*>(&hk_resize), reinterpret_cast<void**>(&g_resize));
    patch_swapchain_slot(vtable, 22, reinterpret_cast<void*>(&hk_present1), reinterpret_cast<void**>(&g_present1));
    patch_swapchain_slot(vtable, 39, reinterpret_cast<void*>(&hk_resize1), reinterpret_cast<void**>(&g_resize1));
    if (g_release_with_game) patch_swapchain_slot(vtable, 2, reinterpret_cast<void*>(&hk_release), reinterpret_cast<void**>(&g_release));
    const ULONG held = render_sc == sc3 ? 2 : 1;
    {
        std::lock_guard lock(g_mutex);
        g_held_refs = held;
        if (g_swapchain) g_reinit = true;  // replaced swapchain: old buffers are released on the render worker
        std::swap(g_swapchain, sc3);
        std::swap(g_queue, queue);
        std::swap(g_render_swapchain, render_sc);
        std::swap(g_render_queue, render_queue);
        g_hwnd = hwnd;
    }
    // Old references are released only after the render thread re-inits (they stay valid until then).
    log("overlay: captured D3D12 swapchain for window %p", static_cast<void*>(hwnd));
}

// ---------------------------------------------------------------- D3D10 / D3D11 swapchains (gyro4all.dll)
//
// Wrappers (ReShade as dxgi.dll or d3d11.dll, Streamline) hand the game proxy swapchains whose Present runs their effects
// and overlays, then the real swapchain's. The panel belongs on the real one, after them, so its UI isn't post-processed
// and isn't hidden under theirs: once the game creates a D3D10/D3D11 device, the real System32 factory's vtable is hooked
// too (ensure_native_factory), and a swapchain created inside another hooked creation (a wrapper creating the real one)
// wins over the proxy around it. DX12 keeps its own rules: lazily hooked native factories are ignored for it.

thread_local int t_create_depth = 0;     // hooked swapchain creations on this thread's stack
thread_local bool t_inner_other = false;  // a D3D10/D3D11 swapchain was captured inside the current outer creation
std::atomic<bool> g_native_lazy{false};   // the System32 factory was hooked for D3D10/D3D11, not as the DX12 path
std::mutex g_factory_mutex;
std::vector<void*> g_game_factory_vtables;  // vtables of the factories the DX12 path hooked (the game's, Streamline's)
void ensure_native_factory();

bool is_d3d12(IUnknown* device) {
    ID3D12CommandQueue* queue = nullptr;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return false;
    queue->Release();
    return true;
}

// A factory the DX12 path never used: the System32 one, hooked only for D3D10/D3D11 under a wrapper.
bool lazy_native_factory(const void* factory) {
    if (!factory || !g_native_lazy) return false;
    std::lock_guard lock(g_factory_mutex);
    return std::find(g_game_factory_vtables.begin(), g_game_factory_vtables.end(), com::vtable_of(factory)) ==
           g_game_factory_vtables.end();
}

// A presenting thread: the static panel's swapchain stopped presenting (kAdoptIdleMs) and the game presents to another
// D3D10/D3D11 one, made where the factory hooks didn't see it: the panel moves to it (and its input to its window).
constexpr ULONGLONG kAdoptIdleMs = 500;
std::atomic<IDXGISwapChain*> g_not_adopted{nullptr};  // the last one found not to be D3D10/D3D11 or windowless

bool adopt_presented(IDXGISwapChain* sc) {
    if (g_overlay.load() || !panel_host::api() || sc == g_not_adopted.load()) return false;
    const ULONGLONG idle = panel_host::target_idle_ms();
    if (idle < kAdoptIdleMs) return false;
    DXGI_SWAP_CHAIN_DESC desc{};
    ID3D11Device* d3d11 = nullptr;
    ID3D10Device* d3d10 = nullptr;
    const char* api = nullptr;
    if (SUCCEEDED(sc->GetDesc(&desc)) && desc.OutputWindow)
        api = SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d3d11))) ? "D3D11"
              : SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d3d10))) ? "D3D10"
                                                               : nullptr;
    if (d3d11) d3d11->Release();
    if (d3d10) d3d10->Release();
    if (!api) {
        g_not_adopted = sc;
        return false;
    }
    log("overlay: the game presents to %s swapchain %s, not the panel's (%p, idle %llu ms): the panel moves to it", api,
        describe(sc).c_str(), panel_host::target(), idle);
    panel_host::set_dxgi_target(sc, desc.OutputWindow, api);
    return true;
}

void on_other_swapchain(IUnknown* created, HWND hwnd) {
    IDXGISwapChain* sc = nullptr;
    if (!created || FAILED(created->QueryInterface(IID_PPV_ARGS(&sc)))) return;
    ID3D11Device* d3d11 = nullptr;
    ID3D10Device* d3d10 = nullptr;
    const char* api = SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d3d11))) ? "D3D11"
                      : SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d3d10))) ? "D3D10"
                                                                       : nullptr;
    if (d3d11) d3d11->Release();
    if (d3d10) d3d10->Release();
    if (!api) {
        log("overlay: swapchain %s is neither D3D12, D3D11 nor D3D10; panel unavailable", describe(sc).c_str());
        sc->Release();
        return;
    }
    void** vtable = com::vtable_of(sc);
    patch_swapchain_slot(vtable, 8, reinterpret_cast<void*>(&hk_present), reinterpret_cast<void**>(&g_present));
    patch_swapchain_slot(vtable, 13, reinterpret_cast<void*>(&hk_resize), reinterpret_cast<void**>(&g_resize));
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        void** vtable3 = com::vtable_of(sc3);
        patch_swapchain_slot(vtable3, 22, reinterpret_cast<void*>(&hk_present1), reinterpret_cast<void**>(&g_present1));
        patch_swapchain_slot(vtable3, 38, reinterpret_cast<void*>(&hk_set_color_space1), reinterpret_cast<void**>(&g_set_color_space1));
        patch_swapchain_slot(vtable3, 39, reinterpret_cast<void*>(&hk_resize1), reinterpret_cast<void**>(&g_resize1));
        sc3->Release();
    }
    log("overlay: %s swapchain %s: the panel is GyroLib's static one, drawn by gyro4all.dll", api, describe(sc).c_str());
    panel_host::set_dxgi_target(sc, hwnd, api);
    sc->Release();
    ensure_native_factory();  // the next swapchain (resolution change, device reset) is caught under any wrapper
}

// Every swapchain made through a hooked factory (or D3D1x CreateDeviceAndSwapChain): DX12 goes to on_swapchain as it
// always did; D3D10/D3D11 to gyro4all.dll, innermost first.
void swapchain_created(const void* factory, IUnknown* device, HWND hwnd, IUnknown* created) {
    const bool nested = t_create_depth > 0;
    if (is_d3d12(device)) {
        if (!lazy_native_factory(factory)) on_swapchain(device, hwnd, created);
        return;
    }
    if (!nested && t_inner_other) {
        if (!panel_host::is_dxgi_target(created))
            log("overlay: swapchain %s not used: the panel draws on the real swapchain inside it", describe(created).c_str());
        return;
    }
    if (nested) t_inner_other = true;
    on_other_swapchain(created, hwnd);
}

void release_job() {
    t_releasing = true;  // our own Releases below go through hk_release
    gl_overlay* overlay = g_overlay.load();
    if (overlay && g_renderer_ready && !g_off_worker) {
        const int32_t r = gl_overlay_dx12_shutdown(overlay);
        log("overlay: renderer shutdown (%d)%s%s", r, r != GL_OK ? ": " : "", r != GL_OK ? gl_overlay_error() : "");
    }
    g_renderer_ready = false;
    g_buffers_held = false;
    g_reinit = false;
    IUnknown* objects[4];
    IUnknown* released = nullptr;
    IDXGISwapChain3* native = nullptr;  // under Streamline's proxy: its references left are logged
    {
        std::lock_guard lock(g_mutex);
        native = native_of(g_swapchain);
        released = g_swapchain;
        objects[0] = g_render_swapchain, objects[1] = g_render_queue, objects[2] = g_queue, objects[3] = g_swapchain;
        g_render_swapchain = nullptr, g_render_queue = nullptr, g_queue = nullptr, g_swapchain = nullptr;
        g_held_refs = 0;
    }
    ULONG left = 0;
    for (IUnknown* o : objects)
        if (o) left = o->Release();  // the captured swapchain last: what is left of it is the game's or a wrapper's
    const ULONG native_left = native ? native->Release() : 0;
    log("overlay: %s: released swapchain %p (%lu references left; native swapchain %p: %lu)", g_release_reason,
        static_cast<void*>(released), left, static_cast<void*>(native), native_left);
    t_releasing = false;
}

void release_captured(const char* reason) {
    g_release_reason = reason;
    if (GetCurrentThreadId() != g_render_thread && on_worker(&release_job)) return;
    g_off_worker = true;  // renderer failed or worker stuck: release only our own references
    release_job();
    g_off_worker = false;
}

// The game released the captured swapchain: when only our and GyroLib's references are left, let go right away, as if
// the panel weren't there.
ULONG STDMETHODCALLTYPE hk_release(IUnknown* self) {
    const ULONG r = original_of(self, 2, g_release)(self);
    if (t_releasing || r == 0) return r;
    ULONG held = 0;
    {
        std::lock_guard lock(g_mutex);
        if (self != static_cast<IUnknown*>(g_swapchain)) return r;
        held = g_held_refs;
    }
    if (r <= held + 2) log("overlay: captured swapchain released: %lu references left (ours and GyroLib's: %lu)", r, held);
    if (r == held) release_captured("the game released its swapchain");
    return r;
}

// Fallback: before the game creates a swapchain for the captured window, let go of the old one (a window has one
// swapchain at a time).
void release_before_create(HWND hwnd) {
    if (!g_release_with_game) return;
    {
        std::lock_guard lock(g_mutex);
        if (!g_swapchain || g_hwnd != hwnd) return;
    }
    release_captured("the game creates a new swapchain for this window");
}

HRESULT STDMETHODCALLTYPE hk_create_swapchain(IDXGIFactory* f, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** out) {
    if (desc && !lazy_native_factory(f)) release_before_create(desc->OutputWindow);
    if (t_create_depth == 0) t_inner_other = false;
    ++t_create_depth;
    unhook_swapchain_slots();
    const HRESULT hr = original_of(f, 10, g_create_swapchain)(f, device, desc, out);
    rehook_swapchain_slots();
    --t_create_depth;
    if (FAILED(hr)) log("overlay: the game's CreateSwapChain failed (0x%08lX)", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && out && *out && desc) swapchain_created(f, device, desc->OutputWindow, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_create_swapchain_hwnd(IDXGIFactory2* f, IUnknown* device, HWND hwnd,
                                                   const DXGI_SWAP_CHAIN_DESC1* desc,
                                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* output,
                                                   IDXGISwapChain1** out) {
    if (!lazy_native_factory(f)) release_before_create(hwnd);
    if (t_create_depth == 0) t_inner_other = false;
    ++t_create_depth;
    unhook_swapchain_slots();
    const HRESULT hr = original_of(f, 15, g_create_swapchain_hwnd)(f, device, hwnd, desc, fs, output, out);
    rehook_swapchain_slots();
    --t_create_depth;
    if (FAILED(hr)) log("overlay: the game's CreateSwapChainForHwnd failed (0x%08lX)", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && out && *out) swapchain_created(f, device, hwnd, *out);
    return hr;
}

// game_path: a factory of the DX12 path (the game's, Streamline's, or the System32 one in system mode), as opposed to the
// System32 factory hooked only for D3D10/D3D11 (ensure_native_factory).
void hook_factory(void* factory, bool game_path = true) {
    if (!factory) return;
    IDXGIFactory2* f2 = nullptr;
    if (FAILED(static_cast<IUnknown*>(factory)->QueryInterface(IID_PPV_ARGS(&f2)))) return;
    void** vtable = *reinterpret_cast<void***>(f2);
    if (game_path) {
        std::lock_guard lock(g_factory_mutex);
        for (void* v : {static_cast<void*>(vtable), static_cast<void*>(com::vtable_of(factory))})
            if (std::find(g_game_factory_vtables.begin(), g_game_factory_vtables.end(), v) == g_game_factory_vtables.end())
                g_game_factory_vtables.push_back(v);
    }
    patch_vtable(vtable, 10, reinterpret_cast<void*>(&hk_create_swapchain), reinterpret_cast<void**>(&g_create_swapchain));
    patch_vtable(vtable, 15, reinterpret_cast<void*>(&hk_create_swapchain_hwnd),
                 reinterpret_cast<void**>(&g_create_swapchain_hwnd));
    f2->Release();
}

// The game called its own dxgi factory import (the default path): Streamline's factory is then left alone.
std::atomic<bool> g_import_factory_seen{false};

HRESULT WINAPI hk_create_factory(REFIID riid, void** out) {
    g_import_factory_seen = true;  // before the call: a factory made through Streamline inside it is the import's
    const HRESULT hr = g_create_factory(riid, out);
    if (SUCCEEDED(hr) && out) hook_factory(*out);
    return hr;
}

HRESULT WINAPI hk_create_factory1(REFIID riid, void** out) {
    g_import_factory_seen = true;  // before the call: a factory made through Streamline inside it is the import's
    const HRESULT hr = g_create_factory1(riid, out);
    if (SUCCEEDED(hr) && out) hook_factory(*out);
    return hr;
}

HRESULT WINAPI hk_create_factory2(UINT flags, REFIID riid, void** out) {
    g_import_factory_seen = true;  // before the call: a factory made through Streamline inside it is the import's
    const HRESULT hr = g_create_factory2(flags, riid, out);
    if (SUCCEEDED(hr) && out) hook_factory(*out);
    return hr;
}

// ---------------------------------------------------------------- D3D10 / D3D11 device creation
//
// Import hooks: a D3D10/D3D11 device is the moment to hook the real factory under any wrapper (before the game's swapchain
// exists), and D3D1xCreateDeviceAndSwapChain makes its swapchain through a factory of its own that the game never saw.

using D3D11CreateDevice_t = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
                                             UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using D3D11CreateDeviceAndSwapChain_t = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                         const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*,
                                                         IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
                                                         ID3D11DeviceContext**);
using D3D10CreateDevice_t = HRESULT(WINAPI*)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT, ID3D10Device**);
using D3D10CreateDeviceAndSwapChain_t = HRESULT(WINAPI*)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT,
                                                         DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D10Device**);
using D3D10CreateDevice1_t = HRESULT(WINAPI*)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, D3D10_FEATURE_LEVEL1, UINT,
                                              ID3D10Device1**);
using D3D10CreateDeviceAndSwapChain1_t = HRESULT(WINAPI*)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT,
                                                          D3D10_FEATURE_LEVEL1, UINT, DXGI_SWAP_CHAIN_DESC*,
                                                          IDXGISwapChain**, ID3D10Device1**);
D3D11CreateDevice_t g_d3d11_create = nullptr;
D3D11CreateDeviceAndSwapChain_t g_d3d11_create_sc = nullptr;
D3D10CreateDevice_t g_d3d10_create = nullptr;
D3D10CreateDeviceAndSwapChain_t g_d3d10_create_sc = nullptr;
D3D10CreateDevice1_t g_d3d10_create1 = nullptr;
D3D10CreateDeviceAndSwapChain1_t g_d3d10_create_sc1 = nullptr;

// The export itself when the import hook had no original yet (a delay-load resolved after the hook was registered).
template <class T>
T real(T& original, const char* dll, const char* function) {
    if (!original)
        if (HMODULE m = GetModuleHandleA(dll)) original = reinterpret_cast<T>(GetProcAddress(m, function));
    return original;
}

void begin_create() {
    ensure_native_factory();
    if (t_create_depth == 0) t_inner_other = false;
    ++t_create_depth;
    unhook_swapchain_slots();
}

void end_create(HRESULT hr, const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    rehook_swapchain_slots();
    --t_create_depth;
    if (SUCCEEDED(hr) && desc && out && *out) swapchain_created(nullptr, nullptr, desc->OutputWindow, *out);
}

HRESULT WINAPI hk_d3d11_create(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE s, UINT f, const D3D_FEATURE_LEVEL* l, UINT n,
                               UINT v, ID3D11Device** d, D3D_FEATURE_LEVEL* o, ID3D11DeviceContext** c) {
    ensure_native_factory();
    auto fn = real(g_d3d11_create, "d3d11.dll", "D3D11CreateDevice");
    return fn ? fn(a, t, s, f, l, n, v, d, o, c) : E_FAIL;
}

HRESULT WINAPI hk_d3d11_create_sc(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE s, UINT f, const D3D_FEATURE_LEVEL* l,
                                  UINT n, UINT v, const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** sc, ID3D11Device** d,
                                  D3D_FEATURE_LEVEL* o, ID3D11DeviceContext** c) {
    auto fn = real(g_d3d11_create_sc, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
    begin_create();
    const HRESULT hr = fn ? fn(a, t, s, f, l, n, v, desc, sc, d, o, c) : E_FAIL;
    end_create(hr, desc, sc);
    return hr;
}

HRESULT WINAPI hk_d3d10_create(IDXGIAdapter* a, D3D10_DRIVER_TYPE t, HMODULE s, UINT f, UINT v, ID3D10Device** d) {
    ensure_native_factory();
    auto fn = real(g_d3d10_create, "d3d10.dll", "D3D10CreateDevice");
    return fn ? fn(a, t, s, f, v, d) : E_FAIL;
}

HRESULT WINAPI hk_d3d10_create_sc(IDXGIAdapter* a, D3D10_DRIVER_TYPE t, HMODULE s, UINT f, UINT v,
                                  DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** sc, ID3D10Device** d) {
    auto fn = real(g_d3d10_create_sc, "d3d10.dll", "D3D10CreateDeviceAndSwapChain");
    begin_create();
    const HRESULT hr = fn ? fn(a, t, s, f, v, desc, sc, d) : E_FAIL;
    end_create(hr, desc, sc);
    return hr;
}

HRESULT WINAPI hk_d3d10_create1(IDXGIAdapter* a, D3D10_DRIVER_TYPE t, HMODULE s, UINT f, D3D10_FEATURE_LEVEL1 l, UINT v,
                                ID3D10Device1** d) {
    ensure_native_factory();
    auto fn = real(g_d3d10_create1, "d3d10_1.dll", "D3D10CreateDevice1");
    return fn ? fn(a, t, s, f, l, v, d) : E_FAIL;
}

HRESULT WINAPI hk_d3d10_create_sc1(IDXGIAdapter* a, D3D10_DRIVER_TYPE t, HMODULE s, UINT f, D3D10_FEATURE_LEVEL1 l,
                                   UINT v, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** sc, ID3D10Device1** d) {
    auto fn = real(g_d3d10_create_sc1, "d3d10_1.dll", "D3D10CreateDeviceAndSwapChain1");
    begin_create();
    const HRESULT hr = fn ? fn(a, t, s, f, l, v, desc, sc, d) : E_FAIL;
    end_create(hr, desc, sc);
    return hr;
}

// ---------------------------------------------------------------- window messages

// While the panel has the mouse, the game's GetCursorPos keeps returning the position from when it took it: Unreal's
// menus (Slate) poll the cursor every frame instead of waiting for WM_MOUSEMOVE, so its pointer moved behind the panel.
// Only the game modules' imports are patched; the panel reads the real position.
using GetCursorPos_t = BOOL(WINAPI*)(LPPOINT);
GetCursorPos_t g_get_cursor_pos = nullptr;
std::atomic<bool> g_mouse_captured{false};

BOOL WINAPI hk_get_cursor_pos(LPPOINT point) {
    static std::atomic<bool> frozen{false};
    static POINT held{};
    GetCursorPos_t real = g_get_cursor_pos ? g_get_cursor_pos : &GetCursorPos;
    if (!g_mouse_captured.load(std::memory_order_relaxed)) {
        frozen = false;
        return real(point);
    }
    if (!frozen.exchange(true) && !real(&held)) frozen = false;
    if (point) *point = held;
    return TRUE;
}

bool is_mouse(UINT m) { return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_INPUT; }
bool is_keyboard(UINT m) { return (m >= WM_KEYFIRST && m <= WM_KEYLAST); }

LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const bool menu_key = (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) &&
                          g_menu_vk != 0 && wp == g_menu_vk;
    const bool alt_f4 = msg == WM_SYSKEYDOWN && wp == VK_F4;
    if (gl_overlay* overlay = g_overlay.load(); overlay && !menu_key) {
        const uint32_t cap = gl_overlay_win32_message(overlay, hwnd, msg, wp, lp);
        if (((cap & GL_OVERLAY_CAPTURE_MOUSE) && is_mouse(msg)) || ((cap & GL_OVERLAY_CAPTURE_KEYBOARD) && is_keyboard(msg) && !alt_f4))
            return 0;  // consumed by the panel (GyroLib already cleaned up WM_INPUT)
    } else if (const g4a_api* panel = panel_host::api(); panel && !menu_key) {
        const uint32_t cap = panel->window_message(hwnd, msg, wp, lp);
        if (((cap & G4A_CAPTURE_MOUSE) && is_mouse(msg)) || ((cap & G4A_CAPTURE_KEYBOARD) && is_keyboard(msg) && !alt_f4)) {
            if (msg == WM_INPUT) DefWindowProcW(hwnd, msg, wp, lp);  // a consumed WM_INPUT still needs its cleanup
            return 0;
        }
    }
    return CallWindowProcW(g_wndproc, hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- settings gate

bool panel_enabled_in_ini(const std::wstring& path, std::string& why) {
    std::ifstream in(path);
    if (!in) {
        why = "no gyrolib.ini yet (GyroLib defaults: F10 + Create/Options)";
        return true;
    }
    std::string line, key = "F10", pad = "1";
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("ui.menu_key=", 0) == 0) key = line.substr(12);
        else if (line.rfind("ui.gamepad_menu_shortcut=", 0) == 0) pad = line.substr(25);
    }
    why = "ui.menu_key=" + key + " ui.gamepad_menu_shortcut=" + pad;
    return !key.empty() || (pad != "0" && !pad.empty());
}

bool g_enabled = false;

// Protected exes (inline hook mode): hooking the dxgi.dll factory exports inline crashed Gears of War: E-Day,
// where Streamline, ReShade (as dxgi.dll) and RivaTuner already chain through those same exports. Instead,
// patch the vtable of the real System32 factory: every wrapper ends up calling it with the real queue and
// gets the real swapchain back, whose Present is hooked as usual. Runs on its own thread, which starts once
// the loader lock is released (no LoadLibrary/COM in DllMain) and well before the engine creates its RHI.
// lazy (non-null): hooked for D3D10/D3D11 under a wrapper (ensure_native_factory), not as the DX12 path.
DWORD WINAPI hook_system_factory(void* lazy) {
    // A dxgi.dll wrapper next to the exe (ReShade) is loaded first, so it keeps its place: System32's loaded by full
    // path before the game's own dxgi import takes the name "dxgi.dll", the game then gets System32's and the wrapper
    // never loads (Mass Effect LE: no ReShade). The wrapper loads System32's itself.
    if (!GetModuleHandleW(L"dxgi.dll")) {
        wchar_t local[MAX_PATH];
        const DWORD len = GetModuleFileNameW(nullptr, local, MAX_PATH);
        wchar_t* slash = len ? wcsrchr(local, L'\\') : nullptr;
        if (slash && lstrcpynW(slash + 1, L"dxgi.dll", MAX_PATH - int(slash + 1 - local)) &&
            GetFileAttributesW(local) != INVALID_FILE_ATTRIBUTES)
            log("overlay: the game's own dxgi.dll (a wrapper, e.g. ReShade) %s first",
                LoadLibraryW(local) ? "loaded" : "could not be loaded");
    }
    wchar_t path[MAX_PATH];
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    lstrcpynW(path + n, L"\\dxgi.dll", MAX_PATH - int(n));
    HMODULE dxgi = LoadLibraryW(path);
    using CreateFactory2_t = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto create = dxgi ? reinterpret_cast<CreateFactory2_t>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
    IDXGIFactory2* factory = nullptr;
    const HRESULT hr = create ? create(0, IID_PPV_ARGS(&factory)) : E_NOINTERFACE;
    if (FAILED(hr) || !factory) {
        log("overlay: system DXGI factory not created (0x%08lx); panel unavailable", static_cast<unsigned long>(hr));
        return 0;
    }
    hook_factory(factory, lazy == nullptr);
    factory->Release();
    log("overlay: system DXGI factory vtable hooked%s (CreateSwapChain %s, CreateSwapChainForHwnd %s)",
        lazy ? " for D3D10/D3D11 under any wrapper" : "", g_create_swapchain ? "ok" : "failed",
        g_create_swapchain_hwnd ? "ok" : "failed");
    return 0;
}

// Default (import-table) mode only: the game makes a D3D10/D3D11 device or swapchain. Synchronous, on the game's thread,
// before its swapchain exists.
bool g_native_allowed = false;
std::atomic<bool> g_native_requested{false};

void ensure_native_factory() {
    if (!g_native_allowed || g_native_requested.exchange(true)) return;
    g_native_lazy = true;
    hook_system_factory(reinterpret_cast<void*>(1));
}

// NVIDIA Streamline's sl.interposer.dll hands the game proxy DXGI objects (factory and swapchain) whose Present runs
// frame generation and then the real swapchain's. Exes whose protector resolves dxgi itself (RE Requiem: the factory
// import is never called) get no swapchain from the default path, and hooking the real System32 factory puts the panel
// below Streamline (crashes and hangs with DLSS-G and REFramework in RE Requiem). So, in import-table mode, the
// interposer's CreateDXGIFactory* exports and slUpgradeInterface are hooked inline as soon as it loads (loader
// notification), and the first Streamline factory decides: if the game's own factory import was called before, the
// panel stays there (the UE games, unchanged); else it hooks Streamline's proxy factory, the one the game uses.
// OverlayFactory=streamline forces this, OverlayFactory=game turns it off (diagnostic overrides).
CreateFactory_t g_sl_create_factory = nullptr, g_sl_create_factory1 = nullptr;
CreateFactory2_t g_sl_create_factory2 = nullptr;
using SlUpgradeInterface_t = int(__cdecl*)(void**);  // sl::Result (0 = eOk) slUpgradeInterface(void** baseInterface)
SlUpgradeInterface_t g_sl_upgrade = nullptr;
std::atomic<bool> g_sl_hooked{false};
std::atomic<int> g_sl_factories{0};
std::atomic<int> g_sl_mode{0};  // 0 = not decided yet, 1 = the panel uses Streamline's factory, -1 = it doesn't

bool use_streamline(const char* via) {
    int mode = g_sl_mode.load();
    if (mode == 0) {
        const int decided = g_import_factory_seen ? -1 : 1;
        if (g_sl_mode.compare_exchange_strong(mode, decided)) {
            if (decided > 0) {
                g_release_with_game = true;
                log("overlay: the game makes its DXGI factory through Streamline (%s), not its own dxgi import: the panel "
                    "uses Streamline's factory", via);
            } else {
                log("overlay: Streamline's %s after the game's own dxgi factory import: the panel stays on the import", via);
            }
        }
        mode = g_sl_mode.load();
    }
    return mode > 0;
}

void hook_sl_factory(void* factory, const char* via) {
    if (!factory || !use_streamline(via)) return;
    // The first few are logged with the module that owns the vtable: it must be sl.interposer's (a proxy), else
    // the hook would sit on the real factory again.
    if (g_sl_factories.fetch_add(1) < 4)
        log("overlay: Streamline factory from %s (vtable in %s)", via, module_of(*static_cast<void**>(factory)).c_str());
    hook_factory(factory);
}

HRESULT WINAPI hk_sl_create_factory(REFIID riid, void** out) {
    const HRESULT hr = g_sl_create_factory(riid, out);
    if (SUCCEEDED(hr) && out) hook_sl_factory(*out, "CreateDXGIFactory");
    return hr;
}

HRESULT WINAPI hk_sl_create_factory1(REFIID riid, void** out) {
    const HRESULT hr = g_sl_create_factory1(riid, out);
    if (SUCCEEDED(hr) && out) hook_sl_factory(*out, "CreateDXGIFactory1");
    return hr;
}

HRESULT WINAPI hk_sl_create_factory2(UINT flags, REFIID riid, void** out) {
    const HRESULT hr = g_sl_create_factory2(flags, riid, out);
    if (SUCCEEDED(hr) && out) hook_sl_factory(*out, "CreateDXGIFactory2");
    return hr;
}

int __cdecl hk_sl_upgrade(void** base) {
    const int r = g_sl_upgrade(base);
    if (r != 0 || !base || !*base) return r;
    IUnknown* object = static_cast<IUnknown*>(*base);
    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&factory)))) {
        hook_sl_factory(factory, "slUpgradeInterface");
        factory->Release();
        return r;
    }
    IDXGISwapChain* swapchain = nullptr;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&swapchain)))) {
        swapchain->Release();
        log("overlay: Streamline upgraded a swapchain, not a factory: panel not attached to it");
    }
    return r;
}

void hook_streamline(HMODULE interposer) {
    if (g_sl_hooked.exchange(true)) return;
    struct Export {
        const char* name;
        void* replacement;
        void** original;
    };
    const Export exports[] = {
        {"CreateDXGIFactory", reinterpret_cast<void*>(&hk_sl_create_factory), reinterpret_cast<void**>(&g_sl_create_factory)},
        {"CreateDXGIFactory1", reinterpret_cast<void*>(&hk_sl_create_factory1), reinterpret_cast<void**>(&g_sl_create_factory1)},
        {"CreateDXGIFactory2", reinterpret_cast<void*>(&hk_sl_create_factory2), reinterpret_cast<void**>(&g_sl_create_factory2)},
        {"slUpgradeInterface", reinterpret_cast<void*>(&hk_sl_upgrade), reinterpret_cast<void**>(&g_sl_upgrade)},
    };
    std::string result;
    for (const Export& e : exports) {
        void* target = reinterpret_cast<void*>(GetProcAddress(interposer, e.name));
        const bool ok = target && hooks::hook_function(target, e.replacement, e.original);
        result += std::string(result.empty() ? "" : ", ") + e.name + (ok ? " ok" : target ? " failed" : " missing");
    }
    g_sl_native = reinterpret_cast<SlGetNativeInterface_t>(GetProcAddress(interposer, "slGetNativeInterface"));
    log("overlay: sl.interposer.dll loaded at %p: %s", static_cast<void*>(interposer), result.c_str());
}

bool watch_streamline() { return hooks::watch_dll(L"sl.interposer.dll", &hook_streamline); }

// D3D10, D3D11 and D3D9 games (gyro4all.dll draws their panel): device creation imports. native: also hook the real
// System32 factories under any wrapper (OverlayFactory=game turns that off).
void install_legacy(bool native) {
    g_native_allowed = native;
    hooks::add("d3d11.dll", "D3D11CreateDevice", reinterpret_cast<void*>(&hk_d3d11_create),
               reinterpret_cast<void**>(&g_d3d11_create));
    hooks::add("d3d11.dll", "D3D11CreateDeviceAndSwapChain", reinterpret_cast<void*>(&hk_d3d11_create_sc),
               reinterpret_cast<void**>(&g_d3d11_create_sc));
    hooks::add("d3d10.dll", "D3D10CreateDevice", reinterpret_cast<void*>(&hk_d3d10_create),
               reinterpret_cast<void**>(&g_d3d10_create));
    hooks::add("d3d10.dll", "D3D10CreateDeviceAndSwapChain", reinterpret_cast<void*>(&hk_d3d10_create_sc),
               reinterpret_cast<void**>(&g_d3d10_create_sc));
    hooks::add("d3d10_1.dll", "D3D10CreateDevice1", reinterpret_cast<void*>(&hk_d3d10_create1),
               reinterpret_cast<void**>(&g_d3d10_create1));
    hooks::add("d3d10_1.dll", "D3D10CreateDeviceAndSwapChain1", reinterpret_cast<void*>(&hk_d3d10_create_sc1),
               reinterpret_cast<void**>(&g_d3d10_create_sc1));
    panel_host::install();
}

// Graphics APIs looked up at run time (LoadLibrary + GetProcAddress) by a game module that also imports the API's DLL
// for other functions, so loads_graphics_api_itself doesn't apply: Crysis's CryRenderD3D10.dll imports d3d10.dll's
// shader helpers but gets D3D10CreateDevice and CreateDXGIFactory with GetProcAddress, and CryRenderD3D9.dll
// Direct3DCreate9. The game modules' GetProcAddress hands out the same hooks the import tables get.
using GetProcAddress_t = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddress_t g_get_proc = nullptr;

struct RuntimeExport {
    const char* dll;
    const char* function;
    void* hook;
    void** original;
    std::atomic<bool> logged{false};
};
RuntimeExport g_runtime_exports[] = {
    {"dxgi.dll", "CreateDXGIFactory", reinterpret_cast<void*>(&hk_create_factory),
     reinterpret_cast<void**>(&g_create_factory)},
    {"dxgi.dll", "CreateDXGIFactory1", reinterpret_cast<void*>(&hk_create_factory1),
     reinterpret_cast<void**>(&g_create_factory1)},
    {"dxgi.dll", "CreateDXGIFactory2", reinterpret_cast<void*>(&hk_create_factory2),
     reinterpret_cast<void**>(&g_create_factory2)},
    {"d3d11.dll", "D3D11CreateDevice", reinterpret_cast<void*>(&hk_d3d11_create),
     reinterpret_cast<void**>(&g_d3d11_create)},
    {"d3d11.dll", "D3D11CreateDeviceAndSwapChain", reinterpret_cast<void*>(&hk_d3d11_create_sc),
     reinterpret_cast<void**>(&g_d3d11_create_sc)},
    {"d3d10.dll", "D3D10CreateDevice", reinterpret_cast<void*>(&hk_d3d10_create),
     reinterpret_cast<void**>(&g_d3d10_create)},
    {"d3d10.dll", "D3D10CreateDeviceAndSwapChain", reinterpret_cast<void*>(&hk_d3d10_create_sc),
     reinterpret_cast<void**>(&g_d3d10_create_sc)},
    {"d3d10_1.dll", "D3D10CreateDevice1", reinterpret_cast<void*>(&hk_d3d10_create1),
     reinterpret_cast<void**>(&g_d3d10_create1)},
    {"d3d10_1.dll", "D3D10CreateDeviceAndSwapChain1", reinterpret_cast<void*>(&hk_d3d10_create_sc1),
     reinterpret_cast<void**>(&g_d3d10_create_sc1)},
    {"d3d9.dll", "Direct3DCreate9", nullptr, nullptr},
    {"d3d9.dll", "Direct3DCreate9Ex", nullptr, nullptr},
};

FARPROC WINAPI hk_get_proc_address(HMODULE module, LPCSTR name) {
    if (!g_get_proc) g_get_proc = &GetProcAddress;  // only a delay-load import was patched so far
    const FARPROC real = g_get_proc(module, name);
    if (!real || !module || IS_INTRESOURCE(name)) return real;
    for (RuntimeExport& e : g_runtime_exports) {
        if (std::strcmp(name, e.function) != 0 || module != GetModuleHandleA(e.dll)) continue;
        void* hook = e.hook;
        if (hook) {
            if (!*e.original) *e.original = reinterpret_cast<void*>(real);
        } else {
            hook = panel_host::runtime_export(e.function, reinterpret_cast<void*>(real));
        }
        if (!hook) return real;
        if (!e.logged.exchange(true))
            log("overlay: %s!%s looked up at run time by %s: hooked", e.dll, e.function,
                mem::module_offset(reinterpret_cast<uintptr_t>(_ReturnAddress())).c_str());
        return reinterpret_cast<FARPROC>(hook);
    }
    return real;
}

void subclass(HWND hwnd) {
    if (g_subclassed || !hwnd) return;
    g_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hk_wndproc)));
    g_subclassed = g_wndproc != nullptr;
    if (g_subclassed) g_subclassed_hwnd = hwnd;
    log("overlay: window messages %s", g_subclassed ? "forwarded" : "NOT forwarded");
}

// The game thread (the windows' thread): give the subclassed window its own procedure back, if it still has ours.
void unsubclass() {
    if (!g_subclassed) return;
    if (IsWindow(g_subclassed_hwnd) &&
        GetWindowLongPtrW(g_subclassed_hwnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&hk_wndproc))
        SetWindowLongPtrW(g_subclassed_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_wndproc));
    g_subclassed = false;
    g_subclassed_hwnd = nullptr;
}

// No game module imports a graphics API (regular or delay-load), and no game DLL can load later: the game loads DXGI
// and its D3D DLL itself (LoadLibrary + GetProcAddress, Metro 2033 Redux), so the import-table factory hooks would never
// see its swapchain; the System32 factory's vtable is hooked instead, as with OverlayFactory=system. D3D9, OpenGL and
// Vulkan imports keep the import-table path (panel_host's hooks).
bool loads_graphics_api_itself() {
    if (hooks::modular()) return false;
    static const char* const apis[] = {"dxgi.dll",     "d3d9.dll",     "d3d10.dll",    "d3d10_1.dll",
                                       "d3d11.dll",    "d3d12.dll",    "opengl32.dll", "vulkan-1.dll"};
    for (const hooks::ImportedDll& dll : hooks::imported_dlls())
        for (const char* api : apis)
            if (_stricmp(dll.name.c_str(), api) == 0) return false;
    return true;
}

}  // namespace

void install(const std::wstring& settings_path) {
    std::string why;
    g_enabled = panel_enabled_in_ini(settings_path, why);
    if (!g_enabled) {
        log("overlay: disabled by gyrolib.ini (%s); no graphics hooks installed", why.c_str());
        return;
    }
    QueryPerformanceFrequency(&g_freq);
    if (!hooks::inline_mode())  // inline, the export itself would be hooked: the panel's own reads too
        hooks::add("user32.dll", "GetCursorPos", reinterpret_cast<void*>(&hk_get_cursor_pos),
                   reinterpret_cast<void**>(&g_get_cursor_pos));
    // OverlayFactory= (diagnostic override, normally empty): "streamline" = only Streamline's factory, "game" = never
    // Streamline's, "system" = the real System32 factory (crashed/hung in RE Requiem with Streamline and REFramework).
    const std::string factory = settings::get_string(L"OverlayFactory");
    const bool force_streamline = _stricmp(factory.c_str(), "streamline") == 0;
    const bool system_factory = _stricmp(factory.c_str(), "system") == 0;
    const bool no_streamline = _stricmp(factory.c_str(), "game") == 0;
    if (!factory.empty() && !force_streamline && !system_factory && !no_streamline)
        log("overlay: OverlayFactory=%s not known (streamline, game, system); ignored", factory.c_str());
    if (force_streamline) {
        g_sl_mode = 1;
        g_release_with_game = true;
        const bool ok = watch_streamline();
        log("overlay: enabled (%s); OverlayFactory=streamline: %s", why.c_str(),
            ok ? "hooking Streamline's factory when sl.interposer.dll loads" : "cannot watch DLL loads; panel unavailable");
        return;
    }
    const bool loads_api = factory.empty() && !hooks::inline_mode() && loads_graphics_api_itself();
    if (hooks::inline_mode() || system_factory || loads_api) {
        if (HANDLE thread = CreateThread(nullptr, 0, &hook_system_factory, nullptr, 0, nullptr)) CloseHandle(thread);
        log("overlay: enabled (%s); %s: hooking the system DXGI factory vtable", why.c_str(),
            system_factory ? "OverlayFactory=system"
            : loads_api    ? "the game imports no graphics API (it loads DXGI itself)"
                           : "protected exe");
        return;
    }
    // Registered for every game module; in modular builds the RHI module loads later and is patched then.
    install_legacy(!no_streamline);
    hooks::add("dxgi.dll", "CreateDXGIFactory", reinterpret_cast<void*>(&hk_create_factory),
               reinterpret_cast<void**>(&g_create_factory));
    hooks::add("dxgi.dll", "CreateDXGIFactory1", reinterpret_cast<void*>(&hk_create_factory1),
               reinterpret_cast<void**>(&g_create_factory1));
    hooks::add("dxgi.dll", "CreateDXGIFactory2", reinterpret_cast<void*>(&hk_create_factory2),
               reinterpret_cast<void**>(&g_create_factory2));
    hooks::add("kernel32.dll", "GetProcAddress", reinterpret_cast<void*>(&hk_get_proc_address),
               reinterpret_cast<void**>(&g_get_proc));
    if (no_streamline) g_sl_mode = -1;
    else if (!watch_streamline()) log("overlay: cannot watch DLL loads: Streamline's factory not available to the panel");
    log("overlay: enabled (%s); DXGI factory hooks registered%s", why.c_str(),
        no_streamline ? " (OverlayFactory=game: not Streamline's)" : ", and Streamline's when sl.interposer.dll loads");
}

void on_present(PresentFn fn) { g_on_present = fn; }

void attach_bridge(gl_context* ctx) {
    if (g_bridge_tried) return;
    g_bridge_tried = true;
    g_bridge = gl_overlay_create(ctx, GL_OVERLAY_ABI_VERSION);
    if (!g_bridge) log("overlay: no Steam Input mouse row in the panel (GyroLib's bridge: %s)", gl_overlay_error());
    else log("overlay: GyroLib's Steam Input mouse row enabled in the gyro4all.dll panel (its bridge, without a renderer)");
}

bool attach(gl_context* ctx) {
    if (!g_enabled) return false;
    if (panel_host::api()) {  // the static panel came first (D3D10/D3D11/D3D9): one frontend per context
        if (HWND moved = panel_host::follow_window()) {
            unsubclass();
            subclass(moved);
        }
        attach_bridge(ctx);
        return true;
    }
    HWND hwnd = nullptr;
    {
        std::lock_guard lock(g_mutex);
        hwnd = g_swapchain ? g_hwnd : nullptr;
    }
    if (!hwnd && !g_overlay.load()) {  // no DX12 swapchain: a D3D10/D3D11/D3D9 game's panel, once it has a target
        // A D3D10/D3D11 swapchain the game doesn't present to (Alan Wake 2 makes a D3D11 one ~2 s before its D3D12 one,
        // MODLOG 2026-10-30) gets no panel yet: one frontend per context, so it would keep it from the DX12 swapchain.
        if (panel_host::target() && (!panel_host::target_has_presented() || panel_host::target_idle_ms() >= kAdoptIdleMs))
            return false;
        if (!panel_host::attach(ctx)) return false;
        subclass(panel_host::window());
        attach_bridge(ctx);
        return true;
    }
    if (!hwnd) return false;
    if (!g_overlay.load()) {
        gl_overlay* overlay = gl_overlay_create(ctx, GL_OVERLAY_ABI_VERSION);
        if (!overlay) {
            log("overlay: gl_overlay_create failed: %s", gl_overlay_error());
            g_enabled = false;
            return false;
        }
        g_overlay = overlay;
        log("overlay: settings panel attached");
    }
    subclass(hwnd);
    return true;
}

void process() {
    if (gl_overlay* overlay = g_overlay.load()) gl_overlay_process(overlay);
    else if (g_bridge) gl_overlay_process(g_bridge);
}

bool available() {
    if (g_overlay.load()) return !g_renderer_failed;
    const g4a_api* panel = panel_host::api();
    return panel && panel->ready();
}

void draw(SDL_Gamepad* pad) { panel_host::frame(pad); }

void poll_menu_key(gl_context* ctx, bool focused) {
    gl_overlay* overlay = g_overlay.load();
    const g4a_api* panel = overlay ? nullptr : panel_host::api();
    const uint32_t key = ctx ? gl_get_menu_key(ctx) : 0;  // F1..F24 as 1..24, 0 = disabled
    g_menu_vk = (key >= 1 && key <= 24) ? UINT(VK_F1 + key - 1) : 0;
    if ((overlay || panel) && ctx && (gl_panel_open(ctx) != 0) != g_last_panel_open) {
        g_last_panel_open = gl_panel_open(ctx) != 0;
        log("overlay: panel state %s", g_last_panel_open ? "open" : "closed");
    }
    if ((!overlay && !panel) || !g_menu_vk) return;
    const bool down = focused && (GetAsyncKeyState(int(g_menu_vk.load())) & 0x8000) != 0;
    if (panel) {  // GyroLib's static panel toggles itself on the configured key's first press
        if (down != g_menu_key_down) {
            panel->function_key(key, down ? 1 : 0, 0);
            if (down)
                log("overlay: F%u pressed (gyro4all.dll panel, renderer %s)", key, panel->ready() ? "ready" : "not ready");
        }
        g_menu_key_down = down;
        return;
    }
    if (down && !g_menu_key_down) {
        const uint32_t open = gl_panel_open(ctx) ? 0 : 1;
        const int32_t r = g_renderer_failed ? GL_UNAVAILABLE : gl_overlay_set_open(overlay, open);
        log("overlay: F%u pressed: request %s (%d)%s%s", gl_get_menu_key(ctx), open ? "open" : "close", r,
            g_renderer_failed ? ", renderer failed" : "", g_renderer_ready ? "" : ", renderer not ready");
    }
    g_menu_key_down = down;
}

bool gyrolib_panel() { return g_overlay.load() != nullptr; }

unsigned capture() {
    unsigned cap = 0;
    if (gl_overlay* overlay = g_overlay.load()) cap = gl_overlay_capture(overlay);
    else if (const g4a_api* panel = panel_host::api()) cap = panel->capture();
    g_mouse_captured = (cap & GL_OVERLAY_CAPTURE_MOUSE) != 0;
    return cap;
}

}  // namespace gyro4all::overlay
