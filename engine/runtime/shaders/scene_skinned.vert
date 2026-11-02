#version 450
#extension GL_EXT_multiview : require

// Forward-lit vertex shader for SKINNED geometry.
//
// Pairs with the same scene.frag as scene.vert -- everything downstream of
// gl_Position is identical, so only the vertex stage differs. Skinning runs
// here rather than in a compute prepass because nothing else in this engine
// consumes the deformed positions (no shadow pass, no ray tracing, no
// GPU-side collision), so writing them out to memory would cost bandwidth
// and buy nothing on a tile-based mobile GPU.
//
// Inputs 0-3 match scene.vert exactly; 4 and 5 are the extra 8 bytes that
// make a skinned vertex 32 bytes instead of 24.
//
//   in_joints  : R8G8B8A8_UINT  -- indices into the joint palette
//   in_weights : R8G8B8A8_UNORM -- influences, quantized to sum to exactly 1
//
// instances.inst[gl_InstanceIndex].params.y is the base index of this draw's
// slice of the shared palette buffer, so instanced clones of one character
// each carry their own pose.

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

layout(std430, set = 0, binding = 1) readonly buffer JointPalette {
    mat4 matrices[];
} palette;

struct Instance {
    mat4 model;
    vec4 color;
    vec4 uv_xform;
    vec4 params;
};
layout(std430, set = 0, binding = 2) readonly buffer Instances {
    Instance inst[];
} instances;

layout(push_constant) uniform PC {
    uint view_base;
    uint pad0;
    uint pad1;
    uint pad2;
    vec4 p0;
    vec4 p1;
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;
layout(location = 4) in uvec4 in_joints;
layout(location = 5) in vec4 in_weights;

layout(location = 0) out vec3 v_world_normal;
layout(location = 1) out vec3 v_world_pos;
layout(location = 2) out vec4 v_color;
layout(location = 3) flat out float v_material;
layout(location = 4) out vec2 v_uv;
layout(location = 5) flat out float v_cutoff;

vec3 oct_decode(vec2 e) {
    vec3 n = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

void main() {
    Instance I = instances.inst[gl_InstanceIndex];
    uint view = min(uint(gl_ViewIndex) + pc.view_base, 1u);
    int base = int(I.params.y);

    mat4 skin =
        in_weights.x * palette.matrices[base + int(in_joints.x)] +
        in_weights.y * palette.matrices[base + int(in_joints.y)] +
        in_weights.z * palette.matrices[base + int(in_joints.z)] +
        in_weights.w * palette.matrices[base + int(in_joints.w)];

    mat4 model_skin = I.model * skin;

    vec4 world = model_skin * vec4(in_pos, 1.0);
    v_world_pos = world.xyz;
    // No inverse-transpose here: joint matrices are assumed free of
    // non-uniform scale, which is standard for character rigs and lets the
    // per-vertex cost stay at one mat3 multiply.
    v_world_normal = normalize(mat3(model_skin) * oct_decode(in_normal));
    v_color = I.color * in_color;
    v_material = I.params.x;
    v_cutoff = I.params.z;
    v_uv = I.uv_xform.xy + in_uv * I.uv_xform.zw;
    gl_Position = frame.view_proj[view] * world;
}
