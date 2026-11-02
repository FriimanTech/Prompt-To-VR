#version 450
#extension GL_EXT_multiview : require

// Gameplay effects (FxRenderer). Quads arrive in world space, already facing
// the camera or lying on their surface; the only transform is the eye's.
// Both eyes' matrices ride in the push constant: a multiview pass picks one
// with gl_ViewIndex, a per-eye pass pushes the same matrix twice.

layout(push_constant) uniform FxPC {
    mat4 view_proj[2];
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 3) in float in_shape;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out int v_shape;

void main() {
    v_uv = in_uv;
    v_color = in_color;
    v_shape = int(in_shape + 0.5);
    gl_Position = pc.view_proj[min(gl_ViewIndex, 1)] * vec4(in_pos, 1.0);
}
