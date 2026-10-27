// The panel in D3D10 games: ImGui's DX10 backend on the game's backbuffer, just before its Present. D3D10 has no device
// context state to swap to, so the game's whole pipeline state is captured in a D3D10 state block (every state, render
// targets included) and applied back after drawing. D3D10 has no HDR output: the panel is drawn directly.
#include "panel.hpp"

#include <d3d10_1.h>
#include <dxgi.h>

#include <imgui.h>
#include <imgui_impl_dx10.h>

namespace g4a::dx10 {
namespace {

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

using CreateStateBlock_t = HRESULT(WINAPI*)(ID3D10Device*, D3D10_STATE_BLOCK_MASK*, ID3D10StateBlock**);
using MaskEnableAll_t = HRESULT(WINAPI*)(D3D10_STATE_BLOCK_MASK*);

ID3D10Device* g_device = nullptr;  // AddRef'd
ID3D10StateBlock* g_state = nullptr;
bool g_failed = false;

void shutdown() {
    if (!g_device) return;
    ImGui_ImplDX10_Shutdown();
    release(g_state);
    release(g_device);
    renderer_ready(false);
}

bool init(ID3D10Device* device) {
    // The state block functions are d3d10.dll exports; the System32 copy (a wrapper DLL of the same name may not have them).
    wchar_t path[MAX_PATH];
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    lstrcpynW(path + n, L"\\d3d10.dll", MAX_PATH - int(n));
    HMODULE d3d10 = LoadLibraryW(path);
    auto create = d3d10 ? reinterpret_cast<CreateStateBlock_t>(GetProcAddress(d3d10, "D3D10CreateStateBlock")) : nullptr;
    auto enable_all = d3d10 ? reinterpret_cast<MaskEnableAll_t>(GetProcAddress(d3d10, "D3D10StateBlockMaskEnableAll")) : nullptr;
    D3D10_STATE_BLOCK_MASK mask{};
    if (!create || !enable_all || FAILED(enable_all(&mask)) || FAILED(create(device, &mask, &g_state))) {
        log("D3D10: no state block for the game's state: panel unavailable");
        return false;
    }
    if (!ImGui_ImplDX10_Init(device)) {
        log("ImGui's DX10 backend did not start: panel unavailable");
        release(g_state);
        return false;
    }
    g_device = device;
    g_device->AddRef();
    renderer_ready(true);
    log("D3D10 renderer ready on device %p", static_cast<void*>(device));
    return true;
}

}  // namespace

bool present(IUnknown* swapchain) {
    Lock lock;
    if (g_failed) return false;
    IDXGISwapChain* sc = nullptr;
    ID3D10Device* device = nullptr;
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&sc)))) return false;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&device)))) {
        sc->Release();
        return false;
    }
    if (device != g_device) {
        if (g_device) log("D3D10: the game presents with another device now: renderer restarted");
        shutdown();
        if (!init(device)) {
            g_failed = true;
            device->Release();
            sc->Release();
            return false;
        }
    }
    device->Release();
    present_seen();
    DXGI_SWAP_CHAIN_DESC desc{};
    if (SUCCEEDED(sc->GetDesc(&desc))) backbuffer_size(desc.BufferDesc.Width, desc.BufferDesc.Height, true);

    ImDrawData* data = draw_data();
    ID3D10Texture2D* backbuffer = nullptr;
    ID3D10RenderTargetView* rtv = nullptr;
    if (data && SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) &&
        SUCCEEDED(g_device->CreateRenderTargetView(backbuffer, nullptr, &rtv))) {
        ImGui_ImplDX10_NewFrame();
        g_state->Capture();
        g_device->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX10_RenderDrawData(data);
        g_state->Apply();
        g_state->ReleaseAllDeviceObjects();  // the block keeps no reference to the game's buffers between frames
    }
    release(rtv);
    release(backbuffer);
    sc->Release();
    return true;
}

void before_resize() {}

}  // namespace g4a::dx10
