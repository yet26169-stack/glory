#version 450

// Depth-only prepass vertex shader.
// Renders all opaque geometry into the depth buffer with minimal work.

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 lightSpaceMatrix;
    mat4 lightSpaceMatrix1;
    mat4 lightSpaceMatrix2;
    vec4 cascadeSplits;
} ubo;

// Per-vertex (binding 0)
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;     // unused
layout(location = 2) in vec3 inNormal;    // unused
layout(location = 3) in vec2 inTexCoord;  // unused

// Per-instance (binding 1)
layout(location = 4) in mat4 inModel;

void main() {
    gl_Position = ubo.proj * ubo.view * inModel * vec4(inPosition, 1.0);
}
