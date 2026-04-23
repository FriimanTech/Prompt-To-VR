#version 450

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 1) uniform sampler2D albedo_tex;

void main() {
    vec3 n = normalize(v_normal);
    float l = max(dot(n, normalize(vec3(0.3, 0.8, 0.4))), 0.0);
    vec4 c = texture(albedo_tex, v_uv);
    out_color = vec4(c.rgb * (0.2 + 0.8 * l), c.a);
}
