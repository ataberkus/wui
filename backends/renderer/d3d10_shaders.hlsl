// WUI D3D10 baseline shaders (vs_4_0/ps_4_0): textured indexed triangles,
// encoded-SDR, premultiplied-alpha blending configured on the pipeline.
// No sRGB conversion. Matches the D3D11 reference shader exactly.

cbuffer Viewport : register(b0) {
    float2 viewport_origin;
    float2 viewport_size;
};

cbuffer TextureMode : register(b1) {
    int texture_is_straight;
    float3 texture_mode_pad;
};

struct VsInput {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct VsOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

VsOutput VsMain(VsInput input) {
    VsOutput output;
    // viewport.xy is logical origin; viewport.zw is logical size.
    output.position = float4((input.position - viewport_origin) / viewport_size * float2(2, -2)
                        + float2(-1, 1), 0, 1);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

Texture2D image : register(t0);
SamplerState linear_clamp : register(s0);

float4 PsMain(VsOutput input) : SV_Target {
    // Sampled textures have a declared straight/premultiplied alpha mode.
    float4 texel = image.Sample(linear_clamp, input.uv);
    if (texture_is_straight) texel.rgb *= texel.a;
    return texel * input.color;
}
