#version 450

// ── Bindings ─────────────────────────────────────────────────────────────────
layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 lightSpaceMatrix;
    mat4 lightSpaceMatrix1;
    mat4 lightSpaceMatrix2;
    vec4 cascadeSplits;
} ubo;

layout(std430, binding = 4) readonly buffer BoneMatrices {
    mat4 bones[];
};

// std430 layout: boneBaseIndex@0, outlineScale@4, (8-byte pad), outlineColor@16
layout(push_constant) uniform PC {
    uint  boneBaseIndex;  // offset  0
    float outlineScale;   // offset  4
    // 8 bytes implicit padding (std430 aligns vec4 to 16)
    vec4  outlineColor;   // offset 16
} pc;

// ── Per-vertex attributes — identical to skinned.vert ────────────────────────
layout(location = 0) in vec3  inPosition;
layout(location = 1) in vec3  inColor;
layout(location = 2) in vec3  inNormal;
layout(location = 3) in vec2  inTexCoord;
layout(location = 4) in ivec4 inJoints;
layout(location = 5) in vec4  inWeights;

// ── Per-instance attributes (binding 1) ──────────────────────────────────────
layout(location = 6)  in mat4 inModel;        // locations 6–9
layout(location = 10) in mat4 inNormalMatrix; // locations 10–13
layout(location = 14) in vec4 inTint;
layout(location = 15) in vec4 inParams;
layout(location = 16) in vec4 inTexIndices;

// ── Output to outline.frag ────────────────────────────────────────────────────
layout(location = 0) out vec4 outOutlineColor;

void main() {
    // ── GPU Skinning (copied from skinned.vert) ───────────────────────────────
    uint base = pc.boneBaseIndex;
    mat4 skinMat = mat4(0.0);
    skinMat += bones[base + inJoints.x] * inWeights.x;
    skinMat += bones[base + inJoints.y] * inWeights.y;
    skinMat += bones[base + inJoints.z] * inWeights.z;
    skinMat += bones[base + inJoints.w] * inWeights.w;
    if (dot(inWeights, vec4(1.0)) < 0.0001)
        skinMat = mat4(1.0);

    vec4 skinnedPos    = skinMat * vec4(inPosition, 1.0);
    vec3 skinnedNormal = mat3(skinMat) * inNormal;

    // ── Inflate in local (skinned) space before world transform ───────────────
    // Inflating before inModel avoids non-uniform scale distorting the shell.
    // Scale inflation by clip-space W so outline stays constant screen-space
    // width regardless of camera distance (LoL/SC2 style).
    vec4 tempClip = ubo.proj * ubo.view * inModel * skinnedPos;
    float distScale = tempClip.w * 0.005; // 0.005 tuned for typical MOBA zoom range
    vec3 inflated = skinnedPos.xyz + normalize(skinnedNormal) * pc.outlineScale * distScale;
    vec4 worldPos = inModel * vec4(inflated, 1.0);
    gl_Position   = ubo.proj * ubo.view * worldPos;

    outOutlineColor = pc.outlineColor;
}
