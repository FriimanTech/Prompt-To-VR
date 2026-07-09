#version 450

// Fullscreen-triangle sky pass. No vertex input: the three positions are
// generated from gl_VertexIndex. v_ndc carries clip-space XY so the fragment
// shader can reconstruct a world-space view ray via the inverse view-proj.

layout(location = 0) out vec2 v_ndc;

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_ndc = p * 2.0 - 1.0;                 // (-1,-1), (3,-1), (-1,3)
    gl_Position = vec4(v_ndc, 0.0, 1.0);
}
