#version 450

// Fullscreen triangle for the sky. It sits ON the far plane (z = w) and is
// drawn after the opaque geometry with a LESS_OR_EQUAL depth test, so only
// pixels nothing covered are shaded.
layout(location = 0) out vec2 v_ndc;

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_ndc = p * 2.0 - 1.0;                 // (-1,-1), (3,-1), (-1,3)
    gl_Position = vec4(v_ndc, 1.0, 1.0);
}
