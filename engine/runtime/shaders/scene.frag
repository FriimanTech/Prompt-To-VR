#version 450

// ---------------------------------------------------------------------------
// Forward-lit fragment shader, driven by the scene's environment UBO so the
// sky / sun / ambient / point lights authored by the world-generation agent
// actually shape the look.
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

layout(location = 0) in vec3 v_world_normal;
layout(location = 1) in vec3 v_world_pos;
layout(location = 2) in vec4 v_color;
layout(location = 3) in float v_material;

layout(location = 0) out vec4 out_color;

struct PointLight {
    vec4 pos_range;        // xyz position, w range (m)
    vec4 color_intensity;  // rgb color, w intensity
};

layout(set = 0, binding = 0) uniform EnvUBO {
    vec4 sun_dir;      // xyz travel direction (normalized), w unused
    vec4 sun_color;    // rgb color, w intensity
    vec4 sky_top;      // rgb zenith color, w exposure
    vec4 sky_horizon;  // rgb horizon/fog color, w fog density
    vec4 ambient;      // rgb ground bounce color, w ambient intensity
    vec4 camera_pos;   // xyz camera world pos, w active point-light count
    PointLight points[8];
} env;

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
    vec3 n = normalize(v_world_normal);
    vec3 base = v_color.rgb;
    vec3 cam = env.camera_pos.xyz;
    vec3 to_cam = cam - v_world_pos;
    float dist = length(to_cam);
    vec3 V = dist > 1e-4 ? to_cam / dist : vec3(0.0, 0.0, 1.0);

    // Global exposure (author-controlled), applied before tone mapping.
    float exposure = env.sky_top.w;

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
        base = mix(base, mix(base, env.sky_horizon.rgb, 0.5), major * 0.28);
    }

    // --- subtle low-frequency albedo variation (distance-faded) -------
    float det = vnoise(v_world_pos.xz * 0.15) +
                0.5 * vnoise(v_world_pos.xz * 0.5 + 13.0);
    det = det / 1.5 - 0.5;
    float det_fade = 1.0 - clamp(dist / 40.0, 0.0, 1.0);
    base *= 1.0 + det * 0.06 * det_fade;

    // --- sun (directional): half-Lambert diffuse + Blinn-Phong spec ---
    vec3 L = normalize(-env.sun_dir.xyz);
    float ndl = max(dot(n, L), 0.0);
    float wrap = dot(n, L) * 0.5 + 0.5;
    float diff = mix(ndl, wrap * wrap, 0.5);
    vec3 sun = env.sun_color.rgb * env.sun_color.w * diff;
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(n, H), 0.0), 32.0) * ndl;
    sun += env.sun_color.rgb * env.sun_color.w * spec * 0.15;

    // --- hemisphere ambient (ground bounce -> zenith) -----------------
    float sky_t = n.y * 0.5 + 0.5;
    vec3 amb = mix(env.ambient.rgb, env.sky_top.rgb, sky_t) * env.ambient.w;

    // --- cheap contact AO: darken low + downward-facing fragments -----
    float ao = mix(0.72, 1.0, clamp(v_world_pos.y * 0.5 + 0.5, 0.0, 1.0));
    ao *= mix(0.85, 1.0, clamp(n.y * 0.5 + 0.5, 0.0, 1.0));

    vec3 lit = base * (amb * ao + sun);

    // --- point lights (range-attenuated) ------------------------------
    int count = int(env.camera_pos.w);
    for (int i = 0; i < count && i < 8; ++i) {
        vec3 d = env.points[i].pos_range.xyz - v_world_pos;
        float dd = length(d);
        float range = max(env.points[i].pos_range.w, 0.001);
        float att = clamp(1.0 - dd / range, 0.0, 1.0);
        att *= att;
        if (att > 0.0) {
            vec3 Lp = d / max(dd, 0.001);
            float nl = max(dot(n, Lp), 0.0);
            lit += base * env.points[i].color_intensity.rgb *
                   env.points[i].color_intensity.w * nl * att;
        }
    }

    // --- rim light (view-dependent; hand highlight is stronger) --------
    float rim = pow(1.0 - max(dot(n, V), 0.0), 3.0);
    float rim_strength = (v_material > 1.5 && v_material < 2.5) ? 0.5 : 0.12;
    lit += env.sky_top.rgb * rim * rim_strength;

    // --- distance fog toward the horizon color ------------------------
    float fog = 1.0 - exp(-dist * env.sky_horizon.w);
    lit = mix(lit, env.sky_horizon.rgb, clamp(fog, 0.0, 0.85));

    out_color = vec4(aces(lit * exposure), v_color.a);
}
