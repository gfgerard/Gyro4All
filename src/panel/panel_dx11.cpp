// The panel in D3D11 games: ImGui's DX11 backend, drawn on the game's backbuffer just before its Present.
//
// The game's pipeline state is left exactly as it was: the panel draws inside its own device context state
// (ID3D11DeviceContext1::SwapDeviceContextState), which swaps out ALL of the game's immediate-context state and brings
// it back afterwards (ImGui's own backup misses render targets, hull/domain/compute shaders, UAVs...). Wrappers that
// track state through their own context proxy (ReShade) keep a correct picture. No reference to a backbuffer is kept
// between frames, so ResizeBuffers and swapchain recreation never wait for the panel.
//
// HDR, as GyroLib's DX12 frontend does it: on an scRGB (FP16) or HDR10 (PQ) swapchain the panel is drawn into an SDR
// texture, then composed over the game's image in linear light with the UI white at 203 nits. HDR10 composes against a
// copy of the scene (PQ decode, Rec.709 -> Rec.2020, blend, PQ encode). Pixels outside the panel, and the backbuffer's
// alpha, are never touched.
#include "panel.hpp"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <string>

namespace g4a::dx11 {
namespace {

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

constexpr float kWhiteNits = 203.0f;  // GyroLib's default UI white in HDR
constexpr UINT kColorSpaceScRgb = 1;  // DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
constexpr UINT kColorSpaceHdr10 = 12; // DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020

enum class Mode { Sdr, ScRgb, Hdr10 };
const char* name(Mode m) { return m == Mode::Sdr ? "SDR" : m == Mode::ScRgb ? "scRGB" : "HDR10"; }

ID3D11Device* g_device = nullptr;  // the device the backend runs on (AddRef'd)
ID3D11DeviceContext1* g_context = nullptr;
ID3DDeviceContextState* g_state = nullptr;  // the panel's own pipeline state
bool g_failed = false;

// HDR composition.
bool g_hdr_pipeline = false, g_hdr_failed = false;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps_scrgb = nullptr;
ID3D11PixelShader* g_ps_hdr10 = nullptr;
ID3D11BlendState* g_blend_premultiplied = nullptr;  // scRGB: ONE / INV_SRC_ALPHA, colour only
ID3D11BlendState* g_blend_replace = nullptr;        // HDR10: the shader blends; colour only
ID3D11RasterizerState* g_raster = nullptr;
ID3D11Buffer* g_constants = nullptr;
ID3D11Texture2D* g_panel_texture = nullptr;  // the panel in SDR, premultiplied by ImGui's blending over transparent black
ID3D11RenderTargetView* g_panel_rtv = nullptr;
ID3D11ShaderResourceView* g_panel_srv = nullptr;
ID3D11Texture2D* g_scene_texture = nullptr;  // HDR10: copy of the game's image
ID3D11ShaderResourceView* g_scene_srv = nullptr;
UINT g_target_width = 0, g_target_height = 0;
DXGI_FORMAT g_target_format = DXGI_FORMAT_UNKNOWN;

Mode g_last_mode = Mode::Sdr;
bool g_mode_logged = false;

const char kCompositeHlsl[] = R"(
cbuffer Constants : register(b0) { float white; float3 unused; };  // scRGB: nits / 80, HDR10: nits / 10000
Texture2D<float4> panel_texture : register(t0);
Texture2D<float4> scene_texture : register(t1);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 srgb_to_linear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
// The panel's pixel, un-premultiplied and decoded to linear Rec.709 (1.0 = UI white).
float3 panel_linear(float4 p) { return srgb_to_linear(saturate(p.rgb / p.a)); }
float4 ps_scrgb(float4 pos : SV_Position) : SV_Target {
    float4 p = panel_texture.Load(int3(pos.xy, 0));
    if (p.a <= 0) discard;
    return float4(panel_linear(p) * white * p.a, p.a);
}
static const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
float3 pq_to_linear(float3 n) { float3 p = pow(saturate(n), 1 / m2); return pow(max(p - c1, 0) / (c2 - c3 * p), 1 / m1); }
float3 linear_to_pq(float3 l) { float3 p = pow(saturate(l), m1); return pow((c1 + c2 * p) / (1 + c3 * p), m2); }
static const float3x3 rec709_to_rec2020 = { 0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956 };
float4 ps_hdr10(float4 pos : SV_Position) : SV_Target {
    float4 p = panel_texture.Load(int3(pos.xy, 0));
    if (p.a <= 0) discard;
    float3 ui = mul(rec709_to_rec2020, panel_linear(p)) * white;
    float3 scene = pq_to_linear(scene_texture.Load(int3(pos.xy, 0)).rgb);
    return float4(linear_to_pq(ui * p.a + scene * (1 - p.a)), 1);
}
)";

ID3DBlob* compile(const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(kCompositeHlsl, sizeof(kCompositeHlsl) - 1, "gyro4all_hdr", nullptr, nullptr, entry,
                                  target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr))
        log("D3D11 HDR shader %s did not compile (0x%08lX)%s%s", entry, static_cast<unsigned long>(hr), errors ? ": " : "",
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
    release(errors);
    return code;
}

void release_targets() {
    release(g_panel_srv);
    release(g_panel_rtv);
    release(g_panel_texture);
    release(g_scene_srv);
    release(g_scene_texture);
    g_target_width = g_target_height = 0;
    g_target_format = DXGI_FORMAT_UNKNOWN;
}

void release_hdr_pipeline() {
    release_targets();
    release(g_vs);
    release(g_ps_scrgb);
    release(g_ps_hdr10);
    release(g_blend_premultiplied);
    release(g_blend_replace);
    release(g_raster);
    release(g_constants);
    g_hdr_pipeline = false;
}

bool create_hdr_pipeline() {
    if (g_hdr_pipeline) return true;
    if (g_hdr_failed) return false;
    ID3DBlob* vs = compile("vs", "vs_4_0");
    ID3DBlob* scrgb = compile("ps_scrgb", "ps_4_0");
    ID3DBlob* hdr10 = compile("ps_hdr10", "ps_4_0");
    bool ok = vs && scrgb && hdr10 &&
              SUCCEEDED(g_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
              SUCCEEDED(g_device->CreatePixelShader(scrgb->GetBufferPointer(), scrgb->GetBufferSize(), nullptr, &g_ps_scrgb)) &&
              SUCCEEDED(g_device->CreatePixelShader(hdr10->GetBufferPointer(), hdr10->GetBufferSize(), nullptr, &g_ps_hdr10));
    release(vs);
    release(scrgb);
    release(hdr10);
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                                  D3D11_COLOR_WRITE_ENABLE_BLUE;
    ok = ok && SUCCEEDED(g_device->CreateBlendState(&blend, &g_blend_premultiplied));
    blend.RenderTarget[0].BlendEnable = FALSE;
    ok = ok && SUCCEEDED(g_device->CreateBlendState(&blend, &g_blend_replace));
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(g_device->CreateRasterizerState(&raster, &g_raster));
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 16;
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ok = ok && SUCCEEDED(g_device->CreateBuffer(&cb, nullptr, &g_constants));
    if (!ok) {
        log("D3D11 HDR composition unavailable: the panel is drawn directly (wrong colours on this HDR swapchain)");
        release_hdr_pipeline();
        g_hdr_failed = true;
        return false;
    }
    g_hdr_pipeline = true;
    return true;
}

// The SDR texture the panel is drawn into (and, for HDR10, the copy of the scene), sized like the backbuffer.
bool ensure_targets(const D3D11_TEXTURE2D_DESC& bb, Mode mode) {
    if (bb.Width != g_target_width || bb.Height != g_target_height || bb.Format != g_target_format) release_targets();
    if (!g_panel_texture) {
        D3D11_TEXTURE2D_DESC t{};
        t.Width = bb.Width;
        t.Height = bb.Height;
        t.MipLevels = 1;
        t.ArraySize = 1;
        t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        t.SampleDesc.Count = 1;
        t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_device->CreateTexture2D(&t, nullptr, &g_panel_texture)) ||
            FAILED(g_device->CreateRenderTargetView(g_panel_texture, nullptr, &g_panel_rtv)) ||
            FAILED(g_device->CreateShaderResourceView(g_panel_texture, nullptr, &g_panel_srv))) {
            release_targets();
            return false;
        }
        g_target_width = bb.Width;
        g_target_height = bb.Height;
        g_target_format = bb.Format;
    }
    if (mode == Mode::Hdr10 && !g_scene_texture) {
        D3D11_TEXTURE2D_DESC t = bb;
        t.MipLevels = 1;
        t.ArraySize = 1;
        t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        t.CPUAccessFlags = 0;
        t.MiscFlags = 0;
        if (FAILED(g_device->CreateTexture2D(&t, nullptr, &g_scene_texture)) ||
            FAILED(g_device->CreateShaderResourceView(g_scene_texture, nullptr, &g_scene_srv))) {
            release(g_scene_srv);
            release(g_scene_texture);
            return false;
        }
    }
    return true;
}

// The colour space the game set wins. Without one, DXGI shows the swapchain as SDR (G22/P709) whatever its format, except
// FP16, which is scRGB. So an RGB10A2 swapchain on an HDR desktop is still SDR until the game says otherwise (Mortal Shell,
// UE 4.25: RGB10A2, never calls SetColorSpace1; GyroLib's AUTO rule would have called it HDR10).
Mode pick_mode(DXGI_FORMAT format, uint32_t color_space) {
    if (color_space != G4A_COLOR_SPACE_UNKNOWN)
        return color_space == kColorSpaceScRgb ? Mode::ScRgb : color_space == kColorSpaceHdr10 ? Mode::Hdr10 : Mode::Sdr;
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? Mode::ScRgb : Mode::Sdr;
}

void composite(ID3D11RenderTargetView* backbuffer, const D3D11_TEXTURE2D_DESC& bb, Mode mode) {
    const float constants[4] = {mode == Mode::ScRgb ? kWhiteNits / 80.0f : kWhiteNits / 10000.0f, 0, 0, 0};
    g_context->UpdateSubresource(g_constants, 0, nullptr, constants, 0, 0);
    D3D11_VIEWPORT vp{0, 0, float(bb.Width), float(bb.Height), 0, 1};
    g_context->RSSetViewports(1, &vp);
    g_context->RSSetState(g_raster);
    g_context->OMSetRenderTargets(1, &backbuffer, nullptr);
    const float factor[4] = {};
    g_context->OMSetBlendState(mode == Mode::ScRgb ? g_blend_premultiplied : g_blend_replace, factor, 0xffffffff);
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_vs, nullptr, 0);
    g_context->PSSetShader(mode == Mode::ScRgb ? g_ps_scrgb : g_ps_hdr10, nullptr, 0);
    g_context->PSSetConstantBuffers(0, 1, &g_constants);
    ID3D11ShaderResourceView* views[2] = {g_panel_srv, g_scene_srv};
    g_context->PSSetShaderResources(0, 2, views);
    g_context->Draw(3, 0);
}

void shutdown() {
    if (!g_device) return;
    release_hdr_pipeline();
    ImGui_ImplDX11_Shutdown();
    release(g_state);
    release(g_context);
    release(g_device);
    renderer_ready(false);
}

bool init(ID3D11Device* device) {
    ID3D11Device1* device1 = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) {
        log("D3D11 device %p has no ID3D11Device1 (D3D 11.1 runtime): panel unavailable", static_cast<void*>(device));
        return false;
    }
    device->GetImmediateContext(&context);
    const bool have_context = context && SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&g_context)));
    release(context);
    D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                           ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                           : 0;
    const HRESULT hr = have_context ? device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION,
                                                                        __uuidof(ID3D11Device1), nullptr, &g_state)
                                    : E_NOINTERFACE;
    release(device1);
    if (FAILED(hr)) {
        log("D3D11: no device context state of its own (0x%08lX): panel unavailable", static_cast<unsigned long>(hr));
        release(g_context);
        return false;
    }
    if (!ImGui_ImplDX11_Init(device, g_context)) {
        log("ImGui's DX11 backend did not start: panel unavailable");
        release(g_state);
        release(g_context);
        return false;
    }
    g_device = device;
    g_device->AddRef();
    g_hdr_failed = false;
    g_mode_logged = false;
    renderer_ready(true);
    log("D3D11 renderer ready on device %p (feature level %X%s)", static_cast<void*>(device), unsigned(level),
        flags ? ", single-threaded" : "");
    return true;
}

}  // namespace

bool present(IUnknown* swapchain, uint32_t color_space) {
    Lock lock;
    if (g_failed) return false;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* device = nullptr;
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&sc)))) return false;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&device)))) {
        sc->Release();
        return false;
    }
    if (device != g_device) {
        if (g_device) log("D3D11: the game presents with another device now: renderer restarted");
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
    ID3D11Texture2D* backbuffer = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (!data || FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
        sc->Release();
        return true;
    }
    D3D11_TEXTURE2D_DESC bb{};
    backbuffer->GetDesc(&bb);
    if (FAILED(g_device->CreateRenderTargetView(backbuffer, nullptr, &rtv))) {
        static bool logged = false;
        if (!logged) log("D3D11: no render target view on the backbuffer (format %d): panel not drawn", int(bb.Format));
        logged = true;
        backbuffer->Release();
        sc->Release();
        return true;
    }
    Mode mode = pick_mode(bb.Format, color_space);
    if (mode != Mode::Sdr && (bb.SampleDesc.Count > 1 || !create_hdr_pipeline() || !ensure_targets(bb, mode)))
        mode = Mode::Sdr;
    if (!g_mode_logged || mode != g_last_mode) {
        log("D3D11: backbuffer %ux%u format %d, colour space %s: panel drawn as %s", bb.Width, bb.Height, int(bb.Format),
            color_space == G4A_COLOR_SPACE_UNKNOWN ? "not set by the game" : std::to_string(color_space).c_str(), name(mode));
        g_last_mode = mode;
        g_mode_logged = true;
    }

    ImGui_ImplDX11_NewFrame();  // (re)creates the backend's device objects when needed
    ID3DDeviceContextState* game_state = nullptr;
    g_context->SwapDeviceContextState(g_state, &game_state);
    if (mode == Mode::Sdr) {
        g_context->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(data);
    } else {
        const float clear[4] = {0, 0, 0, 0};
        g_context->ClearRenderTargetView(g_panel_rtv, clear);
        g_context->OMSetRenderTargets(1, &g_panel_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(data);
        if (mode == Mode::Hdr10) g_context->CopyResource(g_scene_texture, backbuffer);
        composite(rtv, bb, mode);
    }
    g_context->ClearState();  // the panel's state object keeps no reference to the game's buffers
    g_context->SwapDeviceContextState(game_state, nullptr);
    release(game_state);
    rtv->Release();
    backbuffer->Release();
    sc->Release();
    return true;
}

void before_resize() {
    Lock lock;
    release_targets();
}

}  // namespace g4a::dx11
