#version 450

// Uses the main scene UBO (binding 0) which contains lightSpaceMatrix
layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 lightSpaceMatrix;
} ubo;

layout(push_constant) uniform PushConstants {
    mat4  model;
    uint  boneBaseIndex;
} pc;

// Bone matrix SSBO — binding 4 (same as scene pass, ring-buffer layout)
layout(std430, binding = 4) readonly buffer BoneMatrices {
    mat4 bones[];
};

layout(location = 0) in vec3  inPosition;
layout(location = 1) in vec3  inColor;    // unused
layout(location = 2) in vec3  inNormal;   // unused
layout(location = 3) in vec2  inTexCoord; // unused
layout(location = 4) in ivec4 inJoints;
layout(location = 5) in vec4  inWeights;

void main() {
    uint base = pc.boneBaseIndex;
    mat4 skinMat = mat4(0.0);
    skinMat += bones[base + inJoints.x] * inWeights.x;
    skinMat += bones[base + inJoints.y] * inWeights.y;
    skinMat += bones[base + inJoints.z] * inWeights.z;
    skinMat += bones[base + inJoints.w] * inWeights.w;
    if (dot(inWeights, vec4(1.0)) < 0.0001)
        skinMat = mat4(1.0);

    gl_Position = ubo.lightSpaceMatrix * pc.model * skinMat * vec4(inPosition, 1.0);
}
