#version 450

// Shadow depth-only vertex shader.
// Transforms vertices into light-space; the GPU writes depth automatically.

layout(push_constant) uniform PC {
    mat4 lightViewProj;  // per-cascade light VP matrix
} pc;

// Per-vertex (binding 0)
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;     // unused, but must match Vertex layout
layout(location = 2) in vec3 inNormal;    // unused
layout(location = 3) in vec2 inTexCoord;  // unused

// Per-instance (binding 1)
layout(location = 4) in mat4 inModel;       // locations 4-7
// locations 8-14: normalMatrix, tint, params, texIndices — unused but bound

void main() {
    gl_Position = pc.lightViewProj * inModel * vec4(inPosition, 1.0);
}
