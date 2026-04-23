#version 450

layout(push_constant) uniform PC {
    mat4 view_proj;
    mat4 model;
    vec4 color;
    vec4 params;    // .x = material_id
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;

layout(location = 0) out vec3 v_world_normal;
layout(location = 1) out vec3 v_world_pos;
layout(location = 2) out vec4 v_color;
layout(location = 3) out float v_material;

void main() {
    vec4 world = pc.model * vec4(in_pos, 1.0);
    v_world_pos = world.xyz;
    v_world_normal = normalize(mat3(pc.model) * in_normal);
    v_color = pc.color;
    v_material = pc.params.x;
    gl_Position = pc.view_proj * world;
}
