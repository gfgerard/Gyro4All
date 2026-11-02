// The DX12 HDR composite pass of the Gyro4All window (panel_dx12.cpp), GyroLib's own overlay_hdr.hpp shader (GyroLib
// 1.1.0's DX12 HDR overlay): the window is rendered into an SDR RGBA8 texture (ImGui's own blending, over transparent
// black), then this pass composites it over a copy of the game's image in linear light, UI white at 203 nits, with the
// blend in the shader (no hardware blending). scRGB (FP16 backbuffer): the copy is used as-is (1.0 = 80 nits, negative
// and >1 values preserved). HDR10 (RGB10A2): the copy is PQ-decoded, the UI is converted Rec.709 -> Rec.2020, and the
// result PQ-encoded. Pixels with no UI are discarded, so the scene copy's bits (and the backbuffer's alpha) pass
// through untouched. Compiled at RUNTIME with D3DCompile, exactly like GyroLib's composer (D3D12 accepts these DXBC
// shaders; a dxc-built DXIL version of this shader removed the device a few dozen frames in, 007 First Light
// 2026-11-01). CMake stringizes this file into dx12_composite_shader.h; the root signature is built in C++
// (panel_dx12.cpp): param 0 is a descriptor table with the two SRVs, param 1 the two 32-bit constants.
Texture2D<float4> panel : register(t0);
Texture2D<float4> scene : register(t1);
cbuffer Settings : register(b0) { float whiteNits; uint pq; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
}
float3 srgbToLinear(float3 c) {
    return float3(c.r <= .04045 ? c.r / 12.92 : pow((c.r + .055) / 1.055, 2.4),
                  c.g <= .04045 ? c.g / 12.92 : pow((c.g + .055) / 1.055, 2.4),
                  c.b <= .04045 ? c.b / 12.92 : pow((c.b + .055) / 1.055, 2.4));
}
float3 pqToNits(float3 c) {
    float3 p = pow(max(c, 0), 1.0 / 78.84375);
    return 10000 * pow(max(p - .8359375, 0) / max(18.8515625 - 18.6875 * p, 1e-6), 1.0 / .1593017578125);
}
float3 nitsToPq(float3 c) {
    float3 p = pow(saturate(c / 10000), .1593017578125);
    return pow((.8359375 + 18.8515625 * p) / (1 + 18.6875 * p), 78.84375);
}
float4 ps(float4 position : SV_Position) : SV_Target {
    int3 at = int3(position.xy, 0);
    float4 ui = panel.Load(at);
    if (ui.a == 0) discard;
    float4 bg = scene.Load(at);
    float3 color = srgbToLinear(saturate(ui.rgb / ui.a)) * whiteNits;
    if (pq != 0) {
        // Linear Rec.709 -> Rec.2020, D65 white in both spaces.
        color = mul(float3x3(.627404, .329283, .043313,
                            .069097, .919540, .011362,
                            .016391, .088013, .895595), color);
        color = nitsToPq(color * ui.a + pqToNits(bg.rgb) * (1 - ui.a));
    } else {
        // scRGB 1.0 = 80 cd/m2. Preserve negative and >1 scene values.
        color = color / 80 * ui.a + bg.rgb * (1 - ui.a);
    }
    return float4(color, bg.a);
}
