// WUI D3D11 baseline shaders: textured indexed triangles, encoded-SDR,
// premultiplied-alpha blending configured on the pipeline. PsMain writes the
// encoded values unchanged (UNORM targets). PsMainSrgb is selected only for
// _SRGB render-target views: it decodes the same encoded result to linear so
// the hardware sRGB re-encode stores the UNORM path's values.

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

// Encoded-SDR premultiplied result shared by both pixel shaders.
float4 shade_encoded(VsOutput input) {
    // Sampled textures have a declared straight/premultiplied alpha mode.
    float4 texel = image.Sample(linear_clamp, input.uv);
    if (texture_is_straight) texel.rgb *= texel.a;
    return texel * input.color;
}

float4 PsMain(VsOutput input) : SV_Target {
    return shade_encoded(input);
}

// IEC 61966-2-1 sRGB decode, the exact inverse of the hardware encode used
// when writing to an _SRGB render-target view.
float3 srgb_to_linear(float3 encoded) {
    const float3 c = saturate(encoded);
    const float3 low = c / 12.92;
    const float3 high = pow((c + 0.055) / 1.055, 2.4);
    return lerp(low, high, step(0.04045, c));
}

float4 PsMainSrgb(VsOutput input) : SV_Target {
    const float4 encoded = shade_encoded(input);
    // The transfer function applies to straight color: unpremultiply,
    // decode, then premultiply again. Alpha stays linear coverage. Zero
    // alpha has no straight color, so its (normally zero) premultiplied
    // color is decoded directly instead of dividing by zero.
    if (encoded.a <= 0) return float4(srgb_to_linear(encoded.rgb), encoded.a);
    const float3 straight = encoded.rgb / encoded.a;
    return float4(srgb_to_linear(straight) * encoded.a, encoded.a);
}
