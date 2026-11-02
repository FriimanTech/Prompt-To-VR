#version 450
#extension GL_EXT_multiview : require

// Gradient sky + sun disk, per eye. The view ray comes from the eye's
// inverse view-projection in the frame uniform block.

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
    vec4 sun_dir;      // xyz travel direction (normalized)
    vec4 sun_color;    // rgb color, w intensity
    vec4 sky_top;      // rgb zenith color, w exposure
    vec4 sky_horizon;  // rgb horizon/fog color, w fog density
    vec4 ambient;
    vec4 misc;
    PointLight points[8];
} frame;

layout(push_constant) uniform PC {
    uint view_base;
    uint pad0;
    uint pad1;
    uint pad2;
    vec4 p0;
    vec4 p1;
} pc;

vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    uint view = min(uint(gl_ViewIndex) + pc.view_base, 1u);
    mat4 ivp = frame.inv_view_proj[view];
    // The viewport flips Y, so this NDC matches what the scene pass sees.
    vec4 np = ivp * vec4(v_ndc, 0.0, 1.0);
    vec4 fp = ivp * vec4(v_ndc, 1.0, 1.0);
    vec3 dir = normalize(fp.xyz / fp.w - np.xyz / np.w);
    float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    float grad = pow(t, 0.65);
    vec3 col = mix(frame.sky_horizon.rgb, frame.sky_top.rgb, grad);
    vec3 to_sun = normalize(-frame.sun_dir.xyz);
    float sd = max(dot(dir, to_sun), 0.0);
    float disk = smoothstep(0.9992, 0.9998, sd);
    float glow = pow(sd, 256.0) * 0.5 + pow(sd, 32.0) * 0.15;
    col += frame.sun_color.rgb * frame.sun_color.w * (disk * 3.0 + glow);
    col = mix(col, frame.sky_horizon.rgb, pow(1.0 - abs(dir.y), 8.0) * 0.3);
    out_color = vec4(aces(col * frame.sky_top.w), 1.0);
}
