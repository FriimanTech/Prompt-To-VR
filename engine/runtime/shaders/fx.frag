#version 450

// Procedural effect shapes from the quad's UV (-1..1 on both axes); see
// FxShape in FxRenderer.h. Additive shapes return premultiplied-looking
// colour with alpha as intensity; alpha shapes (Decal, Puff) blend normally.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in int v_shape;
layout(location = 0) out vec4 out_color;

void main() {
    vec2 p = v_uv;
    float r = length(p);
    float a = 0.0;
    vec3 rgb = v_color.rgb;
    if (v_shape == 0) {            // Glow
        a = exp(-r * r * 4.0) * (1.0 - smoothstep(0.85, 1.0, r));
        rgb = mix(rgb, vec3(1.0), exp(-r * r * 18.0));
    } else if (v_shape == 1) {     // Streak: p.x across, p.y along
        float across = exp(-p.x * p.x * 6.0);
        float along = smoothstep(-1.0, -0.6, p.y) * (1.0 - smoothstep(0.75, 1.0, p.y));
        a = across * along;
        rgb = mix(rgb, vec3(1.0), exp(-p.x * p.x * 30.0) * 0.6);
    } else if (v_shape == 2) {     // Beam
        float core = exp(-p.x * p.x * 40.0);
        float halo = exp(-p.x * p.x * 4.0) * 0.55;
        a = core + halo;
        rgb = mix(rgb, vec3(1.0), core);
    } else if (v_shape == 3) {     // Star
        float ang = atan(p.y, p.x);
        float petals = pow(abs(cos(ang * 3.0)), 6.0);
        a = (petals * (1.0 - r) + exp(-r * r * 10.0)) * (1.0 - smoothstep(0.9, 1.0, r));
        rgb = mix(rgb, vec3(1.0), exp(-r * r * 14.0));
    } else if (v_shape == 4) {     // Decal
        a = 1.0 - smoothstep(0.55, 1.0, r);
        rgb = mix(rgb, rgb * 0.35, 1.0 - smoothstep(0.0, 0.5, r));
    } else {                       // Puff
        a = exp(-r * r * 3.0) * (1.0 - smoothstep(0.8, 1.0, r));
    }
    a *= v_color.a;
    if (a < 0.004) discard;
    out_color = vec4(rgb, clamp(a, 0.0, 1.0));
}
