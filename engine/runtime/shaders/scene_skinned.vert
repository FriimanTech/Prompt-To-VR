#version 450

// Forward-lit vertex shader for SKINNED geometry.
//
// Pairs with the same scene.frag as scene.vert -- everything downstream of
// gl_Position is identical, so only the vertex stage differs. Skinning runs
// here rather than in a compute prepass because nothing else in this engine
// consumes the deformed positions (no shadow pass, no ray tracing, no
// GPU-side collision), so writing them out to memory would cost bandwidth
// and buy nothing on a tile-based mobile GPU.
//
// Inputs 0-2 match scene.vert exactly; 3 and 4 are the extra 8 bytes that
// make a SkinnedVertex 48 bytes instead of 40.
//
//   in_joints  : R8G8B8A8_UINT  -- indices into the joint palette
//   in_weights : R8G8B8A8_UNORM -- influences, quantized to sum to exactly 1
//
// pc.params.y carries the base index of this draw's slice of the shared
// palette buffer. It rides in an existing unused push-constant component on
// purpose: the block is already 160 bytes and Vulkan only guarantees 128, so
// it must not grow.

layout(push_constant) uniform PC {
    mat4 view_proj;
    mat4 model;
    vec4 color;
    vec4 params;    // .x = material_id, .y = joint palette base index
} pc;

layout(std430, set = 0, binding = 1) readonly buffer JointPalette {
    mat4 matrices[];
} palette;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_color;
layout(location = 3) in uvec4 in_joints;
layout(location = 4) in vec4 in_weights;

layout(location = 0) out vec3 v_world_normal;
layout(location = 1) out vec3 v_world_pos;
layout(location = 2) out vec4 v_color;
layout(location = 3) out float v_material;

void main() {
    int base = int(pc.params.y);

    mat4 skin =
        in_weights.x * palette.matrices[base + int(in_joints.x)] +
        in_weights.y * palette.matrices[base + int(in_joints.y)] +
        in_weights.z * palette.matrices[base + int(in_joints.z)] +
        in_weights.w * palette.matrices[base + int(in_joints.w)];

    mat4 model_skin = pc.model * skin;

    vec4 world = model_skin * vec4(in_pos, 1.0);
    v_world_pos = world.xyz;
    // No inverse-transpose here: joint matrices are assumed free of
    // non-uniform scale, which is standard for character rigs and lets the
    // per-vertex cost stay at one mat3 multiply. Squash-and-stretch bones
    // would need the full normal matrix.
    v_world_normal = normalize(mat3(model_skin) * in_normal);
    v_color = pc.color * in_color;
    v_material = pc.params.x;
    gl_Position = pc.view_proj * world;
}
