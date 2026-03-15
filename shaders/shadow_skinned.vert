#version 450

// Shadow depth-only vertex shader for GPU-skinned meshes.
// Applies skeletal animation, then transforms into light-space.

layout(push_constant) uniform PC {
    mat4 lightViewProj;   // per-cascade light VP
    uint boneBaseIndex;   // base into bones[] ring buffer
} pc;

layout(std430, binding = 4) readonly buffer BoneMatrices {
    mat4 bones[];
};

// Per-vertex (binding 0) — skinned vertex layout
layout(location = 0) in vec3  inPosition;
layout(location = 1) in vec3  inColor;     // unused
layout(location = 2) in vec3  inNormal;    // unused
layout(location = 3) in vec2  inTexCoord;  // unused
layout(location = 4) in ivec4 inJoints;
layout(location = 5) in vec4  inWeights;

// Per-instance (binding 1)
layout(location = 6) in mat4 inModel;       // locations 6-9

void main() {
    mat4 skin = bones[pc.boneBaseIndex + inJoints.x] * inWeights.x
              + bones[pc.boneBaseIndex + inJoints.y] * inWeights.y
              + bones[pc.boneBaseIndex + inJoints.z] * inWeights.z
              + bones[pc.boneBaseIndex + inJoints.w] * inWeights.w;

    vec4 skinnedPos = skin * vec4(inPosition, 1.0);
    gl_Position = pc.lightViewProj * inModel * skinnedPos;
}
