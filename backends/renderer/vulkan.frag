#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;

layout(set = 0, binding = 0) uniform sampler2D texture_sampler;
layout(push_constant) uniform Parameters {
    layout(offset = 0) vec4 viewport;
    layout(offset = 16) uint texture_is_straight;
} parameters;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 texel = texture(texture_sampler, in_uv);
    if (parameters.texture_is_straight != 0u) texel.rgb *= texel.a;
    out_color = texel * in_color;
}
