#version 450

// ---------------------------------------------------------------------------
// Forward lit fragment shader with per-material branches.
//
//   material 0 : generic lit object (3-point rig + rim + fog)
//   material 1 : floor (adds world-space grid, soft radial fade)
//   material 2 : hand / grab highlight (slightly emissive rim pop)
//
// Lighting rig follows a standard "key + fill + sky" setup used in most
// stylised real-time renderers. Wrapped (half-Lambert) diffuse keeps
// shadow sides from crushing to black, which reads better in VR than
// a physically correct Lambert where half the room is always black.
// ---------------------------------------------------------------------------

layout(location = 0) in vec3 v_world_normal;
layout(location = 1) in vec3 v_world_pos;
layout(location = 2) in vec4 v_color;
layout(location = 3) in float v_material;

layout(location = 0) out vec4 out_color;

// Smooth anti-aliased grid using screen-space derivatives. Returns ~0 on
// the line and ~1 elsewhere. ``cell`` is the grid period in world units.
float grid_factor(vec2 p, float cell) {
    vec2 q = abs(fract(p / cell - 0.5) - 0.5) / fwidth(p / cell);
    float line = min(q.x, q.y);
    return min(line, 1.0);
}

void main() {
    vec3 n = normalize(v_world_normal);

    // --- 3-point light rig (directional) ------------------------------
    vec3 key_dir  = normalize(vec3( 0.35,  0.80,  0.40));
    vec3 fill_dir = normalize(vec3(-0.60,  0.30, -0.50));
    vec3 sky_dir  = vec3(0.0, 1.0, 0.0);

    vec3 key_col  = vec3(1.00, 0.96, 0.88) * 0.95;   // warm sun
    vec3 fill_col = vec3(0.45, 0.55, 0.78) * 0.35;   // cool fill
    vec3 sky_col  = vec3(0.55, 0.68, 0.85) * 0.25;   // zenith
    vec3 ground_col = vec3(0.20, 0.18, 0.15) * 0.30; // bounce

    // Half-Lambert keeps the dark side readable.
    float key  = pow(max(dot(n, key_dir),  0.0) * 0.5 + 0.5, 1.6);
    float fill = max(dot(n, fill_dir), 0.0);
    // Sky/ground gradient: full sky on top, ground under.
    float sky_t = n.y * 0.5 + 0.5;
    vec3 ambient = mix(ground_col, sky_col, sky_t);

    vec3 base = v_color.rgb;

    // Per-material base color adjustments.
    if (v_material > 0.5 && v_material < 1.5) {
        // ---- floor grid ----------------------------------------------
        // Two-layer grid: 1m major lines and 0.25m minor lines for a
        // readable sense of scale without looking like a debug scene.
        float major = 1.0 - grid_factor(v_world_pos.xz, 1.0);
        float minor = 1.0 - grid_factor(v_world_pos.xz, 0.25);
        vec3  major_col = vec3(0.60, 0.67, 0.78);
        vec3  minor_col = vec3(0.32, 0.36, 0.42);
        base = mix(base, minor_col, minor * 0.45);
        base = mix(base, major_col, major * 0.85);

        // Soft radial vignette so the infinite floor fades into the sky.
        float d = length(v_world_pos.xz);
        float fade = 1.0 - smoothstep(8.0, 22.0, d);
        base *= mix(0.35, 1.0, fade);
    }

    vec3 lit = base * (ambient + key_col * key + fill_col * fill);

    // --- Rim light ----------------------------------------------------
    // View vector reconstructed from world position against the fragment
    // depth direction is cheap to approximate by using the world-Z pole
    // as a view proxy (good enough for a rim term in VR stereo).
    vec3 view_proxy = normalize(vec3(0.0, 0.1, 1.0));
    float rim_term = pow(1.0 - max(dot(n, view_proxy), 0.0), 3.0);
    float rim_strength = (v_material > 1.5 && v_material < 2.5) ? 0.55 : 0.18;
    lit += vec3(0.85, 0.92, 1.00) * rim_term * rim_strength;

    // --- Distance fog -------------------------------------------------
    // Exponential fog keeps far objects from popping against the sky.
    float dist = length(v_world_pos);
    float fog  = 1.0 - exp(-dist * 0.018);
    vec3 fog_color = vec3(0.58, 0.70, 0.86);
    lit = mix(lit, fog_color, clamp(fog, 0.0, 0.75));

    out_color = vec4(lit, v_color.a);
}
