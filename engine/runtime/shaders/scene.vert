#version 450
#extension GL_EXT_multiview : require

// Forward-lit vertex shader (static geometry).
//
// Inputs (24-byte packed vertex, see Renderer.cpp GpuVertex):
//   in_pos     : object-space position
//   in_normal  : octahedral-encoded object-space normal (R16G16_SNORM)
//   in_uv      : base-colour UV, normalised over the mesh's UV range
//                (R16G16_UNORM; instance.uv_xform restores the real value)
//   in_color   : COLOR_0 x baseColorFactor (R8G8B8A8_UNORM, linear)
//
// Per draw: instances.inst[gl_InstanceIndex] (model, tint, uv_xform,
// params). Per view: frame.view_proj[gl_ViewIndex + pc.view_base] -- the
// multiview pass draws both eyes at once (view_base 0); a per-eye pass sets
// view_base to the eye.

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

struct Instance {
    mat4 model;
    vec4 color;
    vec4 uv_xform;   // xy = uv offset, zw = uv scale
    vec4 params;     // x = material id, y = joint base, z = alpha cutoff
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
    vec4 world = I.model * vec4(in_pos, 1.0);
    v_world_pos = world.xyz;
    // Proper normal matrix so non-uniformly scaled primitives (stretched
    // spheres/cylinders, thin planes) light correctly.
    mat3 normal_matrix = transpose(inverse(mat3(I.model)));
    v_world_normal = normalize(normal_matrix * oct_decode(in_normal));
    v_color = I.color * in_color;
    v_material = I.params.x;
    v_cutoff = I.params.z;
    v_uv = I.uv_xform.xy + in_uv * I.uv_xform.zw;
    gl_Position = frame.view_proj[view] * world;
}
