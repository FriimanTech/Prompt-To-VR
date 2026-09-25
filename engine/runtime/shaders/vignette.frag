#version 450

// Comfort vignette ("tunnel vision"), after Meta's locomotion guidance:
// darken the periphery while artificial motion happens, with a feathered
// edge, measured in DEGREES of visual angle from the eye's forward axis so
// the same amount of view is occluded whatever the headset's field of view.
//
// Drawn after the geometry pass with alpha blending, reusing the sky pass's
// fullscreen-triangle vertex shader.

layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform VignettePC {
    mat4 inv_proj;
    // x: clear half-angle (radians), y: feather width (radians),
    // z: maximum opacity (0..1), w: unused
    vec4 params;
    vec4 color;
} pc;

void main() {
    // View-space ray for this pixel. The eye looks down -Z in view space.
    vec4 np = pc.inv_proj * vec4(v_ndc, 0.0, 1.0);
    vec4 fp = pc.inv_proj * vec4(v_ndc, 1.0, 1.0);
    vec3 dir = normalize(fp.xyz / fp.w - np.xyz / np.w);
    float angle = acos(clamp(-dir.z, -1.0, 1.0));

    float inner = pc.params.x;
    float feather = max(pc.params.y, 1e-3);
    float t = smoothstep(inner, inner + feather, angle);
    out_color = vec4(pc.color.rgb, t * pc.params.z);
}
