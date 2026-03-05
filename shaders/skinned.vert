#version 450

// ── Uniform / SSBO bindings ──────────────────────────────────────────────────
layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 lightSpaceMatrix;
} ubo;

// Large bone SSBO: MAX_CHARS * MAX_BONES entries (ring-buffer layout).
// boneBaseIndex (from push constant) points to the first bone of this entity.
layout(std430, binding = 4) readonly buffer BoneMatrices {
    mat4 bones[];
};

layout(push_constant) uniform PC {
    uint boneBaseIndex; // base index into bones[] for this entity
} pc;

// ── Vertex attributes — bind-pose geometry + skin data (static buffer) ───────
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in ivec4 inJoints;   // up to 4 bone indices (local to this entity's skeleton)
layout(location = 5) in vec4  inWeights;  // corresponding weights (sum to 1.0)

// ── Per-instance attributes (binding 1 — instance buffer) ────────────────────
layout(location = 6)  in mat4 inModel;         // locations 6-9
layout(location = 10) in mat4 inNormalMatrix;  // locations 10-13
layout(location = 14) in vec4 inTint;
layout(location = 15) in vec4 inParams;    // x=shininess, y=metallic, z=roughness, w=emissive
layout(location = 16) in vec4 inTexIndices;

// ── Outputs to fragment shader ────────────────────────────────────────────────
layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out vec3 fragWorldNormal;
layout(location = 4) out vec4 fragLightSpacePos;
layout(location = 5) out float fragShininess;
layout(location = 6) out float fragMetallic;
layout(location = 7) out float fragRoughness;
layout(location = 8) out float fragEmissive;
layout(location = 9)  flat out int fragDiffuseIdx;
layout(location = 10) flat out int fragNormalIdx;

void main() {
    // ── GPU Skinning: blend bone transforms by weights ────────────────────────
    // Joint indices are local (0-based); offset by boneBaseIndex to reach the
    // correct slot in the shared SSBO.
    uint base = pc.boneBaseIndex;
    mat4 skinMat = mat4(0.0);
    skinMat += bones[base + inJoints.x] * inWeights.x;
    skinMat += bones[base + inJoints.y] * inWeights.y;
    skinMat += bones[base + inJoints.z] * inWeights.z;
    skinMat += bones[base + inJoints.w] * inWeights.w;

    // Fall back to identity if all weights are zero (should not happen)
    if (dot(inWeights, vec4(1.0)) < 0.0001)
        skinMat = mat4(1.0);

    // ── Transform: bind-pose -> skinned local -> world ──────────────────────────
    vec4 skinnedPos    = skinMat * vec4(inPosition, 1.0);
    vec3 skinnedNormal = mat3(skinMat) * inNormal;

    vec4 worldPos = inModel * skinnedPos;
    gl_Position   = ubo.proj * ubo.view * worldPos;

    // Normal into world space using the normal matrix
    fragWorldNormal   = normalize(mat3(inNormalMatrix) * skinnedNormal);
    fragWorldPos      = worldPos.xyz;
    fragColor         = inColor * inTint.rgb;
    fragTexCoord      = inTexCoord;
    fragLightSpacePos = ubo.lightSpaceMatrix * worldPos;
    fragShininess     = inParams.x;
    fragMetallic      = inParams.y;
    fragRoughness     = inParams.z;
    fragEmissive      = inParams.w;
    fragDiffuseIdx    = int(inTexIndices.x);
    fragNormalIdx     = int(inTexIndices.y);
}
