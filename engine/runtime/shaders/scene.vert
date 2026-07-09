#version 450

// Forward-lit vertex shader.
//
// Inputs:
//   in_pos     : object-space position
//   in_normal  : object-space normal
//   in_color   : per-vertex baseColor (linear RGBA). Cube path ships
//                white; GLB path ships the source material's
//                pbrMetallicRoughness.baseColorFactor (multiplied by
//                COLOR_0 if the primitive provides it).
//
// Push constants:
//   pc.color   : per-DrawItem tint. We MULTIPLY pc.color * in_color and
//                forward to the fragment shader; this preserves the
//                per-tag tint hints (ground tint, hand highlight, etc.)
//                while letting the source GLB color drive the look.

layout(push_constant) uniform PC {
    mat4 view_proj;
    mat4 model;
    vec4 color;
    vec4 params;    // .x = material_id
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec3 v_world_normal;
layout(location = 1) out vec3 v_world_pos;
layout(location = 2) out vec4 v_color;
layout(location = 3) out float v_material;

void main() {
    vec4 world = pc.model * vec4(in_pos, 1.0);
    v_world_pos = world.xyz;
    // Proper normal matrix so non-uniformly scaled primitives (stretched
    // spheres/cylinders, thin planes) light correctly. Computed in-shader to
    // avoid growing the push-constant block.
    mat3 normal_matrix = transpose(inverse(mat3(pc.model)));
    v_world_normal = normalize(normal_matrix * in_normal);
    v_color = pc.color * in_color;
    v_material = pc.params.x;
    gl_Position = pc.view_proj * world;
}
