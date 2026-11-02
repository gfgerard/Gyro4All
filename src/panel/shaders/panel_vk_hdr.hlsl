// The Vulkan panel's HDR composition (panel_vk.cpp), the same maths as panel_dx11.cpp's: the panel, drawn by ImGui in
// SDR into its own attachment (premultiplied over transparent black), composed over the game's image in linear light
// with the UI white at `white`. Compiled to SPIR-V by tools/compile-vk-shaders.sh into panel_vk_shaders.h.
[[vk::push_constant]] struct Constants { float white; } constants;  // scRGB: nits / 80, HDR10: nits / 10000
[[vk::input_attachment_index(0)]] [[vk::binding(0)]] SubpassInput<float4> panel_input;
[[vk::input_attachment_index(1)]] [[vk::binding(1)]] SubpassInput<float4> scene_input;

float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * 2 - 1, 0, 1);  // Vulkan's clip space: y down, the triangle covers the target either way
}
float3 srgb_to_linear(float3 c) { return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4)); }
// The panel's pixel, un-premultiplied and decoded to linear Rec.709 (1.0 = UI white).
float3 panel_linear(float4 p) { return srgb_to_linear(saturate(p.rgb / p.a)); }
static const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
float3 pq_to_linear(float3 n) { float3 p = pow(saturate(n), 1 / m2); return pow(max(p - c1, 0) / (c2 - c3 * p), 1 / m1); }
float3 linear_to_pq(float3 l) { float3 p = pow(saturate(l), m1); return pow((c1 + c2 * p) / (1 + c3 * p), m2); }
static const float3x3 rec709_to_rec2020 = { 0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956 };

// scRGB: premultiplied output, blended ONE / ONE_MINUS_SRC_ALPHA by the pipeline.
float4 ps_scrgb(float4 pos : SV_Position) : SV_Target {
    float4 p = panel_input.SubpassLoad();
    if (p.a <= 0) discard;
    return float4(panel_linear(p) * constants.white * p.a, p.a);
}
// HDR10 with the game's pixel as an input attachment: decoded, blended in linear light, encoded again (no blending).
float4 ps_hdr10(float4 pos : SV_Position) : SV_Target {
    float4 p = panel_input.SubpassLoad();
    if (p.a <= 0) discard;
    float3 ui = mul(rec709_to_rec2020, panel_linear(p)) * constants.white;
    float3 scene = pq_to_linear(scene_input.SubpassLoad().rgb);
    return float4(linear_to_pq(ui * p.a + scene * (1 - p.a)), 1);
}
// HDR10 without it (the driver refused the usage): premultiplied PQ, blended by the pipeline in PQ. Opaque pixels are
// exact; translucent ones look more opaque than in linear light.
float4 ps_hdr10_pq(float4 pos : SV_Position) : SV_Target {
    float4 p = panel_input.SubpassLoad();
    if (p.a <= 0) discard;
    return float4(linear_to_pq(mul(rec709_to_rec2020, panel_linear(p)) * constants.white) * p.a, p.a);
}
