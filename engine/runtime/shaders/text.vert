#version 450

// World-space SDF text. Vertices arrive already in world space (the CPU
// places each string's quads), so the only transform is the eye's.

layout(push_constant) uniform TextPC {
    mat4 view_proj;
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
    v_uv = in_uv;
    v_color = in_color;
    gl_Position = pc.view_proj * vec4(in_pos, 1.0);
}
