#version 450

// Gradient sky with a soft sun disk/glow. Drawn first each eye with depth
// test/write off so geometry always renders on top; the geometry pass fogs
// distant surfaces toward the same horizon color so the world blends into
// the sky.

layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 out_color;

struct PointLight {
    vec4 pos_range;
    vec4 color_intensity;
};

layout(set = 0, binding = 0) uniform EnvUBO {
    vec4 sun_dir;      // xyz travel direction (normalized)
    vec4 sun_color;    // rgb color, w intensity
    vec4 sky_top;      // rgb zenith color, w exposure
    vec4 sky_horizon;  // rgb horizon/fog color, w fog density
    vec4 ambient;
    vec4 camera_pos;
    PointLight points[8];
} env;

layout(push_constant) uniform SkyPC {
    mat4 inv_view_proj;
} pc;

vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    // Reconstruct the world-space ray for this pixel.
    vec4 np = pc.inv_view_proj * vec4(v_ndc, 0.0, 1.0);
    vec4 fp = pc.inv_view_proj * vec4(v_ndc, 1.0, 1.0);
    vec3 dir = normalize(fp.xyz / fp.w - np.xyz / np.w);

    // Vertical gradient horizon -> zenith (slightly compressed near horizon).
    float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    float grad = pow(t, 0.65);
    vec3 col = mix(env.sky_horizon.rgb, env.sky_top.rgb, grad);

    // Sun disk + glow.
    vec3 to_sun = normalize(-env.sun_dir.xyz);
    float sd = max(dot(dir, to_sun), 0.0);
    float disk = smoothstep(0.9992, 0.9998, sd);
    float glow = pow(sd, 256.0) * 0.5 + pow(sd, 32.0) * 0.15;
    col += env.sun_color.rgb * env.sun_color.w * (disk * 3.0 + glow);

    // Slight haze thickening at the horizon band.
    col = mix(col, env.sky_horizon.rgb, pow(1.0 - abs(dir.y), 8.0) * 0.3);

    // Global exposure (author-controlled), applied before tone mapping so the
    // sky tracks the same brightness as the geometry.
    out_color = vec4(aces(col * env.sky_top.w), 1.0);
}
