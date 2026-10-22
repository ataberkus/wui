// Encoded-SDR, texture alpha converted before multiplying premultiplied vertex color.
float4 viewport : register(c0); // xy logical origin; zw logical size
float4 framebuffer : register(c1); // xy physical dimensions
float4 straight_mode : register(c2); // pixel shader constant

struct VsInput {
    float2 position : POSITION0;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
struct VsOutput {
    float4 position : POSITION0;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

VsOutput VsMain(VsInput input) {
    VsOutput output;
    output.position = float4(
        2 * (input.position.x - viewport.x) / viewport.z - 1 - 1 / framebuffer.x,
        1 - 2 * (input.position.y - viewport.y) / viewport.w + 1 / framebuffer.y,
        0, 1);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

sampler image : register(s0);
float4 PsMain(VsOutput input) : COLOR0 {
    float4 texel = tex2D(image, input.uv);
    texel.rgb *= lerp(1.0f, texel.a, straight_mode.x);
    return texel * input.color;
}
