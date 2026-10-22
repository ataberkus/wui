#version 450

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 premultiplied_color;

layout(push_constant) uniform Parameters {
    layout(offset = 0) vec4 viewport;
    layout(offset = 16) uint texture_is_straight;
} parameters;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

void main() {
    vec2 p = (position - parameters.viewport.xy) / parameters.viewport.zw;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    out_uv = uv;
    out_color = premultiplied_color;
}
