// The panel in D3D12 games: ImGui's DX12 backend, drawn into the game's backbuffer inside its Present (winmm.dll's
// render worker), after GyroLib's DX12 overlay rendered on the same queue. Everything is this panel's own (heaps,
// allocators, command list, fence); the game's and GyroLib's state is never touched. No reference to a backbuffer is
// kept between frames: the render-target views are created lazily per buffer and released before the game's
// ResizeBuffers (dx12_before_resize), so a resize never waits for the panel.
//
// Sync, as GyroLib's DX12 frontend (overlay.cpp): one outstanding submission — every submission is preceded by a
// wait for the previous one (wait_submissions), which keeps the backend's cyclic per-frame buffers and the
// allocators safe to reuse. The wait never blocks forever: a removed device fails immediately, a stalled queue
// times out after 5 s; either way the renderer disables itself (the game keeps running, the panel stops drawing).
//
// HDR, as the D3D11 panel (panel_dx11.cpp) and GyroLib's DX12 frontend do it: on an scRGB (FP16) or HDR10 (PQ,
// RGB10A2) swapchain the window is rendered into an SDR RGBA8 texture, then composited over a copy of the game's
// image in linear light with the UI white at 203 nits (dx12_composite.hlsl, GyroLib's own shader; compiled at runtime
// with D3DCompile like GyroLib's composer, see ensure_composite_pipeline). The blend is in the shader, the copy's
// bits pass through where there is no UI.
#include "panel.hpp"
#include "settings_ui.hpp"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <imgui.h>
#include <imgui_impl_dx12.h>

// Compiled at runtime with D3DCompile, exactly like GyroLib's DX12 HDR composer (detail/overlay_hdr.hpp in GyroLib's
// source), whose identical path works in HDR10 games. A dxc-built DXIL header was used before (compiled by the oldest
// Windows SDK's dxc the glob found); that build removed the device a few dozen frames into the composite (007 First
// Light, 2026-11-01), so the DXIL path is gone. The .hlsl file stays the source of truth; CMake stringizes it into a
// header at build time.
#include "dx12_composite_shader.h"

#include <new>
#include <string>

namespace g4a::dx12 {
namespace {

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

bool wait_submissions();  // defined below, before shutdown() uses it

constexpr float kWhiteNits = 203.0f;  // GyroLib's default UI white in HDR
constexpr UINT kColorSpaceScRgb = 1;  // DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
constexpr UINT kColorSpaceHdr10 = 12; // DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020

enum class Mode { Sdr, ScRgb, Hdr10 };
const char* name(Mode m) { return m == Mode::Sdr ? "SDR" : m == Mode::ScRgb ? "scRGB" : "HDR10"; }

struct Frame {  // one per swapchain buffer: its command allocator and the fence value of its last submission
    ID3D12CommandAllocator* allocator = nullptr;
    uint64_t fence_value = 0;
};

ID3D12Device* g_device = nullptr;       // AddRef'd; everything below is this panel's own, on this device
ID3D12CommandQueue* g_queue = nullptr;  // the game's DIRECT queue (not AddRef'd: winmm.dll keeps it alive while captured)
IDXGISwapChain3* g_swapchain = nullptr;  // compared, not AddRef'd (same)
UINT g_buffer_count = 0;
DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;

Frame* g_frames = nullptr;
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_fence_event = nullptr;  // lazily created
uint64_t g_fence_next = 0;

ID3D12DescriptorHeap* g_rtv_heap = nullptr;  // one RTV per swapchain buffer, created lazily, released before a resize
UINT g_rtv_stride = 0;

// The HDR composite (scRGB and HDR10, one pass: dx12_composite.hlsl): the window in SDR (RGBA8) and a copy of the
// game's image in the backbuffer's format, and the pass that blends the window over the copy in linear light.
ID3D12Resource* g_panel_res = nullptr;
ID3D12Resource* g_scene_res = nullptr;
D3D12_CPU_DESCRIPTOR_HANDLE g_panel_rtv{};
D3D12_CPU_DESCRIPTOR_HANDLE g_panel_srv_cpu{}, g_scene_srv_cpu{};
D3D12_GPU_DESCRIPTOR_HANDLE g_panel_srv_gpu{};
ID3D12DescriptorHeap* g_panel_rtv_heap = nullptr;  // the panel texture's RTV (1 descriptor)
ID3D12DescriptorHeap* g_panel_heap = nullptr;      // shader-visible, 2 SRVs: the panel, then the scene copy
UINT g_srv_stride = 0;
ID3D12RootSignature* g_composite_rs = nullptr;
ID3D12PipelineState* g_composite_pso = nullptr;
DXGI_FORMAT g_composite_format = DXGI_FORMAT_UNKNOWN;  // the backbuffer format the composite PSO was built for
UINT g_panel_width = 0, g_panel_height = 0;
DXGI_FORMAT g_scene_format = DXGI_FORMAT_UNKNOWN;  // the format the scene copy was created with

ID3D12DescriptorHeap* g_imgui_heap = nullptr;  // the ImGui backend's shader-visible heap (its font SRV)
DXGI_FORMAT g_imgui_format = DXGI_FORMAT_UNKNOWN;  // the RTV format the ImGui backend's pipeline was built for
DXGI_FORMAT g_backend_failed_format = DXGI_FORMAT_UNKNOWN;  // a failed backend build is not retried every frame
bool g_imgui_inited = false;
bool g_failed = false;
Mode g_last_mode = Mode::Sdr;
bool g_mode_logged = false;

// Defined below; used by init() to build the ImGui backend before the panel's first frame, with the render-target
// format it will keep (a HDR mode renders the window into the RGBA8 composite texture, not the backbuffer's format:
// building it right avoids a mid-flight Shutdown/Init when the panel first opens).
Mode pick_mode(DXGI_FORMAT format, uint32_t color_space);

D3D12_CPU_DESCRIPTOR_HANDLE rtv(UINT buffer) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(buffer) * g_rtv_stride;
    return h;
}

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

// ImGui's backend allocates SRV descriptors through these (only ever its font texture: the single descriptor of
// g_imgui_heap, handed out forever).
void srv_alloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    *cpu = g_imgui_heap->GetCPUDescriptorHandleForHeapStart();
    *gpu = g_imgui_heap->GetGPUDescriptorHandleForHeapStart();
}
void srv_free(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {}

// Releases everything this panel created. False when the last submission could not be waited for on a healthy
// device (a stalled queue): the GPU objects are left as they are — releasing them while the queue still uses them
// could remove the device — and the renderer must not be re-initialized. A removed device is safe to release.
bool shutdown() {
    if (!g_device) return true;
    if (!wait_submissions() && SUCCEEDED(g_device->GetDeviceRemovedReason())) return false;
    if (g_imgui_inited) {
        // The backend releases its GPU objects (font texture, per-frame vertex/index buffers, pipelines): only when
        // the queue is done with them (wait_submissions above).
        ImGui_ImplDX12_Shutdown();
        g_imgui_inited = false;
    }
    release(g_composite_pso);
    release(g_composite_rs);
    release(g_panel_res);
    release(g_scene_res);
    release(g_panel_rtv_heap);
    release(g_panel_heap);
    g_panel_width = g_panel_height = 0;
    g_composite_format = DXGI_FORMAT_UNKNOWN;
    g_scene_format = DXGI_FORMAT_UNKNOWN;
    release(g_rtv_heap);
    release(g_imgui_heap);
    if (g_frames) {
        for (UINT i = 0; i < g_buffer_count; ++i) release(g_frames[i].allocator);
        delete[] g_frames;
        g_frames = nullptr;
    }
    release(g_list);
    release(g_fence);
    if (g_fence_event) {
        CloseHandle(g_fence_event);
        g_fence_event = nullptr;
    }
    g_fence_next = 0;
    release(g_device);
    g_swapchain = nullptr;
    g_queue = nullptr;
    g_buffer_count = 0;
    g_format = DXGI_FORMAT_UNKNOWN;
    g_imgui_format = DXGI_FORMAT_UNKNOWN;
    g_backend_failed_format = DXGI_FORMAT_UNKNOWN;
    renderer_ready(false);
    return true;
}

// Waits until everything this panel submitted to the game's queue has finished executing (GyroLib's DX12 rule: one
// outstanding submission). Used before reusing an allocator or releasing GPU objects the submitted work may still
// reference. Never blocks forever (GyroLib's wait, overlay.cpp): a removed device fails immediately, a stalled
// queue times out after 5 s; on failure the renderer disables itself (the game keeps running, the panel stops
// drawing) and false is returned.
bool wait_submissions() {
    if (!g_fence || !g_fence_next) return true;
    if (g_device && FAILED(g_device->GetDeviceRemovedReason())) {
        log("DX12: the device was removed (0x%08lX) before the last submission finished: the panel is disabled",
            static_cast<unsigned long>(g_device->GetDeviceRemovedReason()));
        g_failed = true;
        renderer_ready(false);
        return false;
    }
    const uint64_t completed = g_fence->GetCompletedValue();
    if (completed == UINT64_MAX) {
        log("DX12: the fence reports a removed device before the last submission finished: the panel is disabled");
        g_failed = true;
        renderer_ready(false);
        return false;
    }
    if (completed >= g_fence_next) return true;
    if (!g_fence_event) g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fence_event || FAILED(g_fence->SetEventOnCompletion(g_fence_next, g_fence_event))) {
        log("DX12: the fence could not be waited on: the panel is disabled");
        g_failed = true;
        renderer_ready(false);
        return false;
    }
    if (WaitForSingleObject(g_fence_event, 5000) != WAIT_OBJECT_0) {
        log("DX12: the last submission did not finish in 5 s: the panel is disabled");
        g_failed = true;
        renderer_ready(false);
        return false;
    }
    return true;
}

// A GPU-side failure (logged with the device's removed reason when there is one): the renderer is disabled for this
// session — the game keeps running, the panel stops drawing.
bool fail(const char* what) {
    const long removed = g_device ? (long)g_device->GetDeviceRemovedReason() : 0;
    if (removed != 0 && removed != S_OK)
        log("DX12: %s; the device is removed (0x%08lX): the panel is disabled", what, static_cast<unsigned long>(removed));
    else
        log("DX12: %s: the panel is disabled", what);
    g_failed = true;
    renderer_ready(false);
    return false;
}

// One RTV per swapchain buffer. Created lazily on the first present for a swapchain, released before the game's
// ResizeBuffers: the RTVs hold the only references to the buffers this panel keeps.
bool ensure_targets(IDXGISwapChain3* sc) {
    if (g_rtv_heap) return true;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, g_buffer_count, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtv_heap)))) return false;
    g_rtv_stride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < g_buffer_count; ++i) {
        ID3D12Resource* buffer = nullptr;
        if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&buffer)))) {
            release(g_rtv_heap);
            return false;
        }
        g_device->CreateRenderTargetView(buffer, nullptr, rtv(i));  // returns void
        buffer->Release();  // the RTV keeps the reference it needs
    }
    return true;
}

void release_targets() {
    release(g_rtv_heap);
    release(g_panel_res);
    release(g_scene_res);
    g_panel_width = g_panel_height = 0;
}

bool ensure_composite_pipeline(DXGI_FORMAT format) {
    if (g_composite_pso && g_composite_format == format) return true;
    // A failed creation is not retried every frame (a rejected PSO creation is a slow driver call): one attempt per
    // format per session.
    static DXGI_FORMAT failed_format = DXGI_FORMAT_UNKNOWN;
    if (failed_format == format) return false;
    release(g_composite_pso);
    // The root signature (matching dx12_composite.hlsl) is format-independent: param 0 is a descriptor table with the
    // panel's and the scene's SRVs, param 1 the two 32-bit constants {whiteNits, pq}. GyroLib's composer builds the
    // same two parameters with no flags.
    if (!g_composite_rs) {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = {1, &range};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = {0, 0, 2};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        const D3D12_ROOT_SIGNATURE_DESC rs{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob* blob = nullptr;
        ID3DBlob* errors = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        if (errors) {
            log("DX12: composite root signature: %s", (const char*)errors->GetBufferPointer());
            errors->Release();
        }
        if (FAILED(hr) || !blob ||
            FAILED(hr = g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                      IID_PPV_ARGS(&g_composite_rs)))) {
            release(blob);
            failed_format = format;
            log("DX12: the composite root signature failed to build (0x%08lX)", static_cast<unsigned long>(hr));
            return false;
        }
        release(blob);
    }
    // The shaders, compiled at runtime like GyroLib's composer (D3D12 takes these DXBC shaders; the dxc/DXIL build
    // removed the device in 007).
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(kG4aDx12CompositeShader, sizeof(kG4aDx12CompositeShader) - 1, "Gyro4All HDR", nullptr,
                            nullptr, "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors);
    if (SUCCEEDED(hr))
        hr = D3DCompile(kG4aDx12CompositeShader, sizeof(kG4aDx12CompositeShader) - 1, "Gyro4All HDR", nullptr,
                        nullptr, "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors);
    if (errors) {
        log("DX12: composite shader: %s", (const char*)errors->GetBufferPointer());
        errors->Release();
    }
    if (FAILED(hr) || !vs || !ps) {
        release(vs);
        release(ps);
        failed_format = format;
        log("DX12: the composite shaders failed to compile (0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }
    // No hardware blending: the shader composites against the scene copy and writes the backbuffer's own alpha. The
    // descriptor mirrors GyroLib's composer field for field (a zero-init FillMode is not a valid mode: E_INVALIDARG;
    // the write mask must not stay zero either).
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_composite_rs;
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = format;
    pso.SampleDesc.Count = 1;
    hr = g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_composite_pso));
    release(vs);
    release(ps);
    if (FAILED(hr)) {
        release(g_composite_pso);
        failed_format = format;
        log("DX12: the composite pipeline failed to build (0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }
    g_composite_format = format;
    return true;
}

// The HDR composite's targets, sized like the backbuffer: the window in SDR (RGBA8) and a copy of the game's image in
// the backbuffer's format. The panel's RTV lives in its own heap; the two SRVs (panel, then the scene copy) fill
// g_panel_heap.
bool ensure_panel_target(UINT width, UINT height, DXGI_FORMAT format) {
    if (g_panel_res && g_panel_width == width && g_panel_height == height && g_scene_format == format) return true;
    // A failed creation is not retried every frame (two committed 4K resources per attempt): one attempt per size per
    // session.
    static UINT failed_w = 0, failed_h = 0;
    static DXGI_FORMAT failed_fmt = DXGI_FORMAT_UNKNOWN;
    if (failed_w == width && failed_h == height && failed_fmt == format) return false;
    release(g_panel_res);
    release(g_scene_res);
    if (!g_panel_rtv_heap) {
        const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_panel_rtv_heap)))) return false;
        g_panel_rtv = g_panel_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    }
    if (!g_panel_heap) {
        const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_panel_heap)))) return false;
        g_srv_stride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g_panel_srv_cpu = g_panel_heap->GetCPUDescriptorHandleForHeapStart();
        g_scene_srv_cpu = g_panel_srv_cpu;
        g_scene_srv_cpu.ptr += g_srv_stride;
        g_panel_srv_gpu = g_panel_heap->GetGPUDescriptorHandleForHeapStart();
    }
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC tex{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format, {1, 0},
                            D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                 IID_PPV_ARGS(&g_scene_res)))) {
        failed_w = width, failed_h = height, failed_fmt = format;
        return false;
    }
    tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const float clear[4] = {0, 0, 0, 0};
    const D3D12_CLEAR_VALUE cv{DXGI_FORMAT_R8G8B8A8_UNORM, {clear[0], clear[1], clear[2], clear[3]}};
    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 &cv, IID_PPV_ARGS(&g_panel_res)))) {
        release(g_scene_res);
        failed_w = width, failed_h = height, failed_fmt = format;
        return false;
    }
    g_device->CreateRenderTargetView(g_panel_res, nullptr, g_panel_rtv);
    g_device->CreateShaderResourceView(g_panel_res, nullptr, g_panel_srv_cpu);
    g_device->CreateShaderResourceView(g_scene_res, nullptr, g_scene_srv_cpu);
    g_panel_width = width;
    g_panel_height = height;
    g_scene_format = format;
    return true;
}

// (Re)builds the ImGui backend when the render-target format it draws into changes (the SDR backbuffer format, or
// RGBA8 for the composite texture on an HDR swapchain).
bool ensure_backend(ID3D12CommandQueue* queue, DXGI_FORMAT format) {
    if (g_imgui_inited && g_imgui_format == format) return true;
    if (g_backend_failed_format == format) return false;  // a failed build is not retried every frame (slow driver calls)
    if (g_imgui_inited) {
        // Re-creating the backend (the render-target format changed) releases GPU objects the last submitted frame may
        // still use: wait for this panel's submissions first (see wait_submissions).
        if (!wait_submissions()) return false;
        ImGui_ImplDX12_Shutdown();
        g_imgui_inited = false;
    }
    if (!g_imgui_heap) {
        const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_imgui_heap)))) return false;
    }
    ImGui_ImplDX12_InitInfo info{};
    info.Device = g_device;
    info.CommandQueue = queue;
    info.NumFramesInFlight = int(g_buffer_count);
    info.RTVFormat = format;
    info.SrvDescriptorHeap = g_imgui_heap;
    info.SrvDescriptorAllocFn = &srv_alloc;
    info.SrvDescriptorFreeFn = &srv_free;
    if (!ImGui_ImplDX12_Init(&info)) return false;
    // Like GyroLib's DX12 frontend (overlay.cpp builds the device objects right after Init): creating them lazily
    // inside NewFrame defers every failure into the first frame of an open panel; built here, at renderer init, a
    // failure is logged once and the panel just doesn't draw.
    if (!ImGui_ImplDX12_CreateDeviceObjects()) {
        g_backend_failed_format = format;  // not retried every frame
        log("DX12: ImGui's device objects failed to build: the window is not drawn");
        ImGui_ImplDX12_Shutdown();
        g_imgui_inited = false;
        return false;
    }
    g_imgui_inited = true;
    g_imgui_format = format;
    return true;
}

bool init(IDXGISwapChain3* sc, ID3D12CommandQueue* queue, uint32_t color_space) {
    ID3D12Device* device = nullptr;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&device)))) return false;
    ID3D12Device* queue_device = nullptr;
    const bool same_device = SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&queue_device))) && queue_device == device;
    release(queue_device);
    if (!same_device) {
        log("DX12: the swapchain's and the queue's devices differ: the panel is not drawn");
        release(device);
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 d{};
    sc->GetDesc1(&d);
    g_frames = new (std::nothrow) Frame[d.BufferCount];
    bool ok = g_frames != nullptr;
    for (UINT i = 0; ok && i < d.BufferCount; ++i)
        ok = SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frames[i].allocator)));
    ok = ok && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].allocator, nullptr,
                                                   IID_PPV_ARGS(&g_list))) &&
         SUCCEEDED(g_list->Close()) &&
         SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));
    if (ok) {
        g_device = device;
        g_device->AddRef();
        g_swapchain = sc;
        g_queue = queue;
        g_buffer_count = d.BufferCount;
        g_format = d.Format;
        backbuffer_size(d.Width, d.Height, false);
        // The ImGui backend must exist BEFORE the panel's first frame: NewFrame() locks the font atlas when the
        // backend lacks ImGuiBackendFlags_RendererHasTextures (imgui.cpp UpdateFontsNewFrame), and a locked atlas
        // makes GetFontBaked() return NULL on the first text measurement (the 007 crash). Present runs every frame
        // from game start, so initializing here means the flag is set long before F10. As in GyroLib, an HDR swapchain
        // gets the RGBA8 backend from the start: rebuilding an SDR backend when the panel first opens leaves its font
        // and frame resources being replaced at the most fragile point. A failure is remembered (no per-frame retry)
        // and present() disables the renderer on the first drawn frame.
        const Mode initial_mode = pick_mode(d.Format, color_space);
        if (!ensure_backend(queue, initial_mode == Mode::Sdr ? d.Format : DXGI_FORMAT_R8G8B8A8_UNORM))
            log("DX12: the ImGui backend is not ready: the window will not be drawn");
        renderer_ready(true);
        log("DX12 renderer ready (%u buffers, format %d)", d.BufferCount, int(d.Format));
    } else {
        release(device);
        shutdown();  // releases the partially created state
    }
    return ok;
}

// The colour space the game set wins, checked against the backbuffer's real format (GyroLib's Composer::supported):
// HDR10 needs the PQ space on RGB10A2, scRGB the G10 space on FP16. Without a set space, DXGI shows the swapchain as
// SDR whatever its format, except FP16, which is scRGB (panel_dx11.cpp's rule, including its deliberate lack of an
// HDR10 auto-detect: an RGB10A2 swapchain on an HDR desktop is still SDR until the game calls SetColorSpace1 with
// the PQ space).
Mode pick_mode(DXGI_FORMAT format, uint32_t color_space) {
    if (color_space == kColorSpaceHdr10) return format == DXGI_FORMAT_R10G10B10A2_UNORM ? Mode::Hdr10 : Mode::Sdr;
    if (color_space == kColorSpaceScRgb) return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? Mode::ScRgb : Mode::Sdr;
    if (color_space != G4A_COLOR_SPACE_UNKNOWN) return Mode::Sdr;
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? Mode::ScRgb : Mode::Sdr;
}

}  // namespace

bool present(void* swapchain, void* command_queue, uint32_t color_space) {
    Lock lock;
    if (g_failed) return false;
    IDXGISwapChain3* sc = static_cast<IDXGISwapChain3*>(swapchain);
    ID3D12CommandQueue* queue = static_cast<ID3D12CommandQueue*>(command_queue);
    DXGI_SWAP_CHAIN_DESC1 d{};
    sc->GetDesc1(&d);
    if (sc != g_swapchain || queue != g_queue || d.BufferCount != g_buffer_count || d.Format != g_format) {
        if (g_device) log("DX12: a new swapchain/queue or buffer layout: renderer restarted");
        // A shutdown that could not wait for the last submission (stalled queue) leaves the GPU objects in flight:
        // re-initializing on top of them could remove the device, so the renderer stays disabled.
        if (!shutdown() || !init(sc, queue, color_space)) {
            g_failed = true;
            return false;
        }
    }
    present_seen();
    backbuffer_size(d.Width, d.Height, false);

    ImDrawData* data = draw_data();
    // Nothing to draw (panel closed or an empty frame): no GPU work, no fences, no logging.
    if (!data || data->TotalVtxCount == 0 || !ensure_targets(sc)) return true;

    // The composite is the normal path for an scRGB/HDR10 swapchain; the SDR fallback (wrong colours on the HDR
    // swapchain) is only for an MSAA swapchain or a composite that failed to build (each logged once).
    Mode mode = pick_mode(d.Format, color_space);
    if (mode != Mode::Sdr) {
        // Switching SDR -> HDR replaces the backend and may replace the composite resources. Finish the previous
        // submission before any of those objects is released, not just before the command allocator is reused.
        const bool replaces_resources = (g_imgui_inited && g_imgui_format != DXGI_FORMAT_R8G8B8A8_UNORM) ||
                                        (g_composite_pso && g_composite_format != d.Format) ||
                                        (g_panel_res && (g_panel_width != d.Width || g_panel_height != d.Height ||
                                                         g_scene_format != d.Format));
        if (replaces_resources && !wait_submissions()) return false;
        // The composite needs a single-sample swapchain, the pipeline and its targets; each failure is logged once.
        const char* reason = nullptr;
        if (d.SampleDesc.Count > 1) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                log("DX12: no %s composite: the swapchain has %u samples; the window is drawn directly, with wrong "
                    "colours on this HDR swapchain",
                    name(mode), d.SampleDesc.Count);
            }
            mode = Mode::Sdr;
        } else if (!ensure_composite_pipeline(d.Format)) {
            reason = "the composite pipeline failed to build";
        } else if (!ensure_panel_target(d.Width, d.Height, d.Format)) {
            reason = "the composite targets failed to create";
        }
        if (reason) {
            static std::string logged_reason;
            if (logged_reason != reason) {
                logged_reason = reason;
                log("DX12: no %s composite: %s; the window is drawn directly, with wrong colours on this HDR swapchain",
                    name(mode), reason);
            }
            mode = Mode::Sdr;
        }
    }
    if (!g_mode_logged || mode != g_last_mode) {
        log("DX12: backbuffer %ux%u format %d, %u samples, colour space %s: the window drawn as %s", d.Width, d.Height,
            int(d.Format), d.SampleDesc.Count,
            color_space == G4A_COLOR_SPACE_UNKNOWN ? "not set by the game" : std::to_string(color_space).c_str(),
            name(mode));
        g_last_mode = mode;
        g_mode_logged = true;
    }
    if (!ensure_backend(queue, mode == Mode::Sdr ? d.Format : DXGI_FORMAT_R8G8B8A8_UNORM)) {
        g_failed = true;  // the window cannot be drawn: disable the renderer (the panel closes on its heartbeat)
        return false;
    }

    ImGui_ImplDX12_NewFrame();
    const UINT frame = sc->GetCurrentBackBufferIndex();
    if (frame >= g_buffer_count) {
        fail("the swapchain returned an invalid backbuffer index");
        return false;
    }
    Frame& f = g_frames[frame];
    // One outstanding overlay submission (GyroLib's DX12 rule, overlay.cpp): the ImGui backend's cyclic per-frame
    // vertex/index buffers and its dynamic texture updates are only safe to reuse when nothing this panel submitted
    // is still executing — a per-buffer wait lets a cyclic slot be rewritten while the GPU still reads it (GPU fault,
    // device removed, then the null-upload-buffer AV of the 007 crashes of 2026-11-01).
    if (!wait_submissions()) return false;
    if (FAILED(f.allocator->Reset())) return fail("the command allocator could not be reset");
    if (FAILED(g_list->Reset(f.allocator, nullptr))) return fail("the command list could not be reset");
    ID3D12Resource* buffer = nullptr;
    if (FAILED(sc->GetBuffer(frame, IID_PPV_ARGS(&buffer)))) return true;

    ID3D12DescriptorHeap* heaps[] = {g_imgui_heap};
    g_list->SetDescriptorHeaps(1, heaps);
    D3D12_CPU_DESCRIPTOR_HANDLE back_rtv = rtv(frame);
    if (mode == Mode::Sdr) {
        barrier(g_list, buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        g_list->OMSetRenderTargets(1, &back_rtv, FALSE, nullptr);
        ImGui_ImplDX12_RenderDrawData(data, g_list);
        barrier(g_list, buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    } else {
        // The window into the SDR texture (ImGui's own blending, over transparent black), then the composite pass
        // (dx12_composite.hlsl) blends it over a copy of the game's image and writes the backbuffer: scRGB in FP16
        // linear light, HDR10 PQ-decoded, Rec.709 -> Rec.2020 and PQ-encoded.
        barrier(g_list, buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(g_list, g_scene_res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        g_list->CopyResource(g_scene_res, buffer);
        barrier(g_list, g_scene_res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        barrier(g_list, buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float clear[4] = {0, 0, 0, 0};
        g_list->ClearRenderTargetView(g_panel_rtv, clear, 0, nullptr);
        g_list->OMSetRenderTargets(1, &g_panel_rtv, FALSE, nullptr);
        ImGui_ImplDX12_RenderDrawData(data, g_list);
        barrier(g_list, g_panel_res, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_list->OMSetRenderTargets(1, &back_rtv, FALSE, nullptr);
        ID3D12DescriptorHeap* composite_heaps[] = {g_panel_heap};
        g_list->SetDescriptorHeaps(1, composite_heaps);
        g_list->SetGraphicsRootSignature(g_composite_rs);
        g_list->SetPipelineState(g_composite_pso);
        g_list->SetGraphicsRootDescriptorTable(0, g_panel_srv_gpu);
        const struct { float white; uint32_t pq; } constants{kWhiteNits, mode == Mode::Hdr10 ? 1u : 0u};
        g_list->SetGraphicsRoot32BitConstants(1, 2, &constants, 0);
        const D3D12_VIEWPORT vp{0, 0, float(d.Width), float(d.Height), 0, 1};
        const D3D12_RECT rect{0, 0, LONG(d.Width), LONG(d.Height)};
        g_list->RSSetViewports(1, &vp);
        g_list->RSSetScissorRects(1, &rect);
        g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_list->DrawInstanced(3, 1, 0, 0);
        barrier(g_list, g_panel_res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        barrier(g_list, buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }
    if (FAILED(g_list->Close())) return fail("the command list could not be closed");
    ID3D12CommandList* lists[] = {g_list};
    queue->ExecuteCommandLists(1, lists);  // void: a failure surfaces as a removed device (wait_submissions catches it)
    const uint64_t value = ++g_fence_next;
    queue->Signal(g_fence, value);  // void: same
    f.fence_value = value;
    buffer->Release();  // no backbuffer reference is kept between frames
    return true;
}

void before_resize(void* swapchain) {
    Lock lock;
    if (static_cast<void*>(swapchain) != static_cast<void*>(g_swapchain)) return;
    // The RTVs hold the only buffer references: release them only when the GPU is done with them. A wait that
    // cannot finish (a removed device, a stalled queue) disables the renderer; the swapchain is lost or the
    // panel is gone, so there is nothing more to save here.
    if (!wait_submissions()) return;
    release_targets();
}

void lost() {
    Lock lock;
    if (!g_device) return;
    log("DX12: the captured swapchain is gone: renderer shut down");
    shutdown();
}

}  // namespace g4a::dx12
