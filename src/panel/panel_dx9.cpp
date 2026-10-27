// The panel in D3D9 games: ImGui's DX9 backend, in a scene of its own on the game's backbuffer just before Present.
// The game's state comes back from a D3DSBT_ALL state block (ImGui's backend keeps one too) plus its render targets and
// depth surface, which state blocks don't hold. The backend's resources live in D3DPOOL_DEFAULT, so they are released
// before every Reset/ResetEx and recreated on the next frame. D3D9 has no HDR output: the panel is drawn directly.
#include "panel.hpp"

#include <d3d9.h>

#include <imgui.h>
#include <imgui_impl_dx9.h>

namespace g4a::dx9 {
namespace {

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

IDirect3DDevice9* g_device = nullptr;  // AddRef'd
bool g_failed = false;
constexpr DWORD kMaxTargets = 4;

void shutdown() {
    if (!g_device) return;
    ImGui_ImplDX9_Shutdown();
    release(g_device);
    renderer_ready(false);
}

bool init(IDirect3DDevice9* device) {
    if (!ImGui_ImplDX9_Init(device)) {
        log("ImGui's DX9 backend did not start: panel unavailable");
        return false;
    }
    g_device = device;
    g_device->AddRef();
    renderer_ready(true);
    log("D3D9 renderer ready on device %p", static_cast<void*>(device));
    return true;
}

}  // namespace

bool present(IUnknown* unknown) {
    Lock lock;
    if (g_failed) return false;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&device)))) return false;
    if (device != g_device) {
        if (g_device) log("D3D9: the game presents with another device now: renderer restarted");
        shutdown();
        if (!init(device)) {
            g_failed = true;
            device->Release();
            return false;
        }
    }
    present_seen();
    ImDrawData* data = draw_data();
    IDirect3DSurface9* backbuffer = nullptr;
    if (device->TestCooperativeLevel() != D3D_OK || FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer))) {
        device->Release();
        return true;  // device lost: nothing to draw on until the game resets it
    }
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(backbuffer->GetDesc(&desc))) backbuffer_size(desc.Width, desc.Height, false);
    if (data) {
        D3DCAPS9 caps{};
        device->GetDeviceCaps(&caps);
        const DWORD targets = caps.NumSimultaneousRTs >= 1 && caps.NumSimultaneousRTs < kMaxTargets ? caps.NumSimultaneousRTs
                                                                                                    : kMaxTargets;
        IDirect3DSurface9* saved[kMaxTargets] = {};
        IDirect3DSurface9* depth = nullptr;
        for (DWORD i = 0; i < targets; ++i) device->GetRenderTarget(i, &saved[i]);
        device->GetDepthStencilSurface(&depth);
        IDirect3DStateBlock9* state = nullptr;
        if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &state)) && SUCCEEDED(state->Capture())) {
            device->SetRenderTarget(0, backbuffer);
            for (DWORD i = 1; i < targets; ++i) device->SetRenderTarget(i, nullptr);
            device->SetDepthStencilSurface(nullptr);
            device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
            if (SUCCEEDED(device->BeginScene())) {
                ImGui_ImplDX9_NewFrame();
                ImGui_ImplDX9_RenderDrawData(data);
                device->EndScene();
            }
            // Render targets first: setting target 0 resets the viewport, which the state block then brings back.
            for (DWORD i = 0; i < targets; ++i)
                if (i > 0 || saved[0]) device->SetRenderTarget(i, saved[i]);
            device->SetDepthStencilSurface(depth);
            state->Apply();
        }
        release(state);
        release(depth);
        for (IDirect3DSurface9*& s : saved) release(s);
    }
    backbuffer->Release();
    device->Release();
    return true;
}

void before_reset() {
    Lock lock;
    if (g_device) ImGui_ImplDX9_InvalidateDeviceObjects();
}

}  // namespace g4a::dx9
