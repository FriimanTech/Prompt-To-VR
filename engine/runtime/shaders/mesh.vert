#version 450

layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view_proj;
} frame;

layout(push_constant) uniform PC { mat4 model; } pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_uv;

void main() {
    gl_Position = frame.view_proj * pc.model * vec4(in_pos, 1.0);
    v_normal = mat3(pc.model) * in_normal;
    v_uv = in_uv;
}
