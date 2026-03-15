#version 450

// Minimal fragment shader for the stencil-write pass.
// Color writes are masked to zero by the pipeline; this just satisfies
// the two-output render pass so SPIR-V validation is happy.
layout(location = 0) out vec4  outColor;
layout(location = 1) out float outCharDepth;

void main() {
    outColor     = vec4(0.0);
    outCharDepth = gl_FragCoord.z;
}
