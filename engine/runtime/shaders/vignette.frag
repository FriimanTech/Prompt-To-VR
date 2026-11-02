#version 450
#extension GL_EXT_multiview : require

// Comfort vignette: darkens the view outside a clear cone around each eye's
// forward axis. Angles, not screen fractions, so every headset occludes the
// same visual field.

layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 out_color;

struct PointLight {
    vec4 pos_range;
    vec4 color_intensity;
};
layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view_proj[2];
    mat4 inv_view_proj[2];
    mat4 inv_proj[2];
    vec4 camera_pos[2];
    vec4 sun_dir;
    vec4 sun_color;
    vec4 sky_top;
    vec4 sky_horizon;
    vec4 ambient;
    vec4 misc;
    PointLight points[8];
} frame;

layout(push_constant) uniform PC {
    uint view_base;
    uint pad0;
    uint pad1;
    uint pad2;
    vec4 params;  // x = clear half-angle (rad), y = feather (rad), z = opacity
    vec4 color;
} pc;

void main() {
    uint view = min(uint(gl_ViewIndex) + pc.view_base, 1u);
    mat4 ip = frame.inv_proj[view];
    vec4 np = ip * vec4(v_ndc, 0.0, 1.0);
    vec4 fp = ip * vec4(v_ndc, 1.0, 1.0);
    vec3 dir = normalize(fp.xyz / fp.w - np.xyz / np.w);
    float angle = acos(clamp(-dir.z, -1.0, 1.0));
    float inner = pc.params.x;
    float feather = max(pc.params.y, 1e-3);
    float t = smoothstep(inner, inner + feather, angle);
    out_color = vec4(pc.color.rgb, t * pc.params.z);
}
