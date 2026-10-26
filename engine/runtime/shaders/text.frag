#version 450

// Signed-distance-field glyphs with a dark outline, so text reads over any
// background -- a bright sky, a dark corridor, or a panel.
//
// The atlas stores 180/255 on the glyph edge (see TextAtlas). The smoothing
// width comes from fwidth so edges stay one pixel soft at any distance,
// which is what keeps VR text from shimmering as the head moves.

layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 out_color;

const float kEdge = 180.0 / 255.0;
const float kOutline = 0.16;

void main() {
    float d = texture(atlas, v_uv).r;
    float w = max(fwidth(d) * 0.75, 1e-4);
    float body = smoothstep(kEdge - w, kEdge + w, d);
    float shape = smoothstep(kEdge - kOutline - w, kEdge - kOutline + w, d);
    vec3 rgb = mix(vec3(0.02), v_color.rgb, body);
    float a = shape * v_color.a;
    if (a < 0.01) discard;
    out_color = vec4(rgb, a);
}
