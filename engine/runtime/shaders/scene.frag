#version 450
#extension GL_EXT_multiview : require

// ---------------------------------------------------------------------------
// Forward-lit fragment shader, driven by the frame uniform block so the sky /
// sun / ambient / point lights authored by the world-generation agent shape
// the look.
//
// Albedo = base-colour texture (set 1; sRGB, decoded by the sampler) x the
// vertex colour (COLOR_0 x baseColorFactor) x the per-draw tint. Untextured
// meshes and world primitives bind a 1x1 white texture, so they look exactly
// as they did before textures existed.
//
//   material 0 : generic lit object (sun + hemisphere ambient + points)
//   material 1 : floor (adds a soft world-space grid on top of the lit result)
//   material 2 : hand / grab highlight (stronger rim pop)
//   material 3 : unlit marker (teleport reticle) -- emissive, ignores lighting
//
// Wrapped (half-Lambert) diffuse keeps shadow sides from crushing to black,
// which reads better in VR than physically correct Lambert. A cheap in-shader
// contact term + subtle low-frequency albedo variation + an ACES tonemap give
// the scene depth without shadow maps or heavy post.
// ---------------------------------------------------------------------------

// Specialisation constant: the alpha-tested pipeline variant (MASK, and
// BLEND drawn as MASK). Opaque pipelines compile the discard out, which keeps
// early depth testing on tiled GPUs.
layout(constant_id = 0) const bool ALPHA_TEST = false;

layout(location = 0) in vec3 v_world_normal;
layout(location = 1) in vec3 v_world_pos;
layout(location = 2) in vec4 v_color;
layout(location = 3) flat in float v_material;
layout(location = 4) in vec2 v_uv;
layout(location = 5) flat in float v_cutoff;

layout(location = 0) out vec4 out_color;

struct PointLight {
    vec4 pos_range;        // xyz position, w range (m)
    vec4 color_intensity;  // rgb color, w intensity
};

layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view_proj[2];
    mat4 inv_view_proj[2];
    mat4 inv_proj[2];
    vec4 camera_pos[2];    // xyz eye world position
    vec4 sun_dir;          // xyz travel direction (normalized), w unused
    vec4 sun_color;        // rgb color, w intensity
    vec4 sky_top;          // rgb zenith color, w exposure
    vec4 sky_horizon;      // rgb horizon/fog color, w fog density
    vec4 ambient;          // rgb ground bounce color, w ambient intensity
    vec4 misc;             // x = active point-light count
    PointLight points[8];
} frame;

layout(set = 1, binding = 0) uniform sampler2D base_color_tex;

layout(push_constant) uniform PC {
    uint view_base;
    uint pad0;
    uint pad1;
    uint pad2;
    vec4 p0;
    vec4 p1;
} pc;

// Smooth anti-aliased grid using screen-space derivatives. Returns ~0 on the
// line and ~1 elsewhere. ``cell`` is the grid period in world units.
float grid_factor(vec2 p, float cell) {
    vec2 q = abs(fract(p / cell - 0.5) - 0.5) / fwidth(p / cell);
    return min(min(q.x, q.y), 1.0);
}

// Cheap value noise for very-low-frequency albedo variation.
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Narkowicz ACES filmic tonemap approximation.
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec4 albedo = texture(base_color_tex, v_uv) * v_color;
    if (ALPHA_TEST) {
        if (albedo.a < v_cutoff) discard;
    }

    uint view = min(uint(gl_ViewIndex) + pc.view_base, 1u);
    vec3 n = normalize(v_world_normal);
    vec3 base = albedo.rgb;
    vec3 cam = frame.camera_pos[view].xyz;
    vec3 to_cam = cam - v_world_pos;
    float dist = length(to_cam);
    vec3 V = dist > 1e-4 ? to_cam / dist : vec3(0.0, 0.0, 1.0);

    // Global exposure (author-controlled), applied before tone mapping.
    float exposure = frame.sky_top.w;
    float out_alpha = ALPHA_TEST ? albedo.a : 1.0;

    // --- material 3: unlit emissive marker ----------------------------
    if (v_material > 2.5) {
        out_color = vec4(aces(base * 1.2 * exposure), v_color.a);
        return;
    }

    // --- floor grid (blended into albedo before lighting) -------------
    if (v_material > 0.5 && v_material < 1.5) {
        float major = 1.0 - grid_factor(v_world_pos.xz, 1.0);
        float minor = 1.0 - grid_factor(v_world_pos.xz, 0.25);
        // Subtle, tied to the horizon color so it grounds the scene without
        // screaming "debug grid".
        base = mix(base, base * 1.18, minor * 0.20);
        base = mix(base, mix(base, frame.sky_horizon.rgb, 0.5), major * 0.28);
    }

    // --- subtle low-frequency albedo variation (distance-faded) -------
    float det = vnoise(v_world_pos.xz * 0.15) +
                0.5 * vnoise(v_world_pos.xz * 0.5 + 13.0);
    det = det / 1.5 - 0.5;
    float det_fade = 1.0 - clamp(dist / 40.0, 0.0, 1.0);
    base *= 1.0 + det * 0.06 * det_fade;

    // --- sun (directional): half-Lambert diffuse + Blinn-Phong spec ---
    vec3 L = normalize(-frame.sun_dir.xyz);
    float ndl = max(dot(n, L), 0.0);
    float wrap = dot(n, L) * 0.5 + 0.5;
    float diff = mix(ndl, wrap * wrap, 0.5);
    vec3 sun = frame.sun_color.rgb * frame.sun_color.w * diff;
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(n, H), 0.0), 32.0) * ndl;
    sun += frame.sun_color.rgb * frame.sun_color.w * spec * 0.15;

    // --- hemisphere ambient (ground bounce -> zenith) -----------------
    float sky_t = n.y * 0.5 + 0.5;
    vec3 amb = mix(frame.ambient.rgb, frame.sky_top.rgb, sky_t) * frame.ambient.w;

    // --- cheap contact AO: darken low + downward-facing fragments -----
    float ao = mix(0.72, 1.0, clamp(v_world_pos.y * 0.5 + 0.5, 0.0, 1.0));
    ao *= mix(0.85, 1.0, clamp(n.y * 0.5 + 0.5, 0.0, 1.0));

    vec3 lit = base * (amb * ao + sun);

    // --- point lights (range-attenuated) ------------------------------
    int count = int(frame.misc.x);
    for (int i = 0; i < count && i < 8; ++i) {
        vec3 d = frame.points[i].pos_range.xyz - v_world_pos;
        float dd = length(d);
        float range = max(frame.points[i].pos_range.w, 0.001);
        float att = clamp(1.0 - dd / range, 0.0, 1.0);
        att *= att;
        if (att > 0.0) {
            vec3 Lp = d / max(dd, 0.001);
            float nl = max(dot(n, Lp), 0.0);
            lit += base * frame.points[i].color_intensity.rgb *
                   frame.points[i].color_intensity.w * nl * att;
        }
    }

    // --- rim light (view-dependent; hand highlight is stronger) --------
    float rim = pow(1.0 - max(dot(n, V), 0.0), 3.0);
    float rim_strength = (v_material > 1.5 && v_material < 2.5) ? 0.5 : 0.12;
    lit += frame.sky_top.rgb * rim * rim_strength;

    // --- distance fog toward the horizon color ------------------------
    float fog = 1.0 - exp(-dist * frame.sky_horizon.w);
    lit = mix(lit, frame.sky_horizon.rgb, clamp(fog, 0.0, 0.85));

    out_color = vec4(aces(lit * exposure), out_alpha);
}
