#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 lightSpaceMatrix;
    mat4 lightSpaceMatrix1;
    mat4 lightSpaceMatrix2;
    vec4 cascadeSplits;
} ubo;

// Per-vertex attributes (binding 0)
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;

// Per-instance attributes (binding 1)
layout(location = 4)  in mat4 inModel;
layout(location = 8)  in mat4 inNormalMatrix;
layout(location = 12) in vec4 inTint;
layout(location = 13) in vec4 inParams;     // x=shininess, y=metallic, z=roughness, w=emissive
layout(location = 14) in vec4 inTexIndices;  // x=diffuseIdx, y=normalIdx

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out vec3 fragWorldNormal;
layout(location = 4) out vec4 fragLightSpacePos;
layout(location = 5) out float fragShininess;
layout(location = 6) out float fragMetallic;
layout(location = 7) out float fragRoughness;
layout(location = 8) out float fragEmissive;
layout(location = 9) flat out int fragDiffuseIdx;
layout(location = 10) flat out int fragNormalIdx;
layout(location = 11) out vec4 fragLightSpacePos1;
layout(location = 12) out vec4 fragLightSpacePos2;
layout(location = 13) out float fragViewDepth;

void main() {
    vec4 worldPos = inModel * vec4(inPosition, 1.0);
    vec4 viewPos  = ubo.view * worldPos;
    gl_Position   = ubo.proj * viewPos;

    fragColor          = inColor * inTint.rgb;
    fragTexCoord       = inTexCoord;
    fragWorldPos       = worldPos.xyz;
    fragWorldNormal    = mat3(inNormalMatrix) * inNormal;
    fragLightSpacePos  = ubo.lightSpaceMatrix  * worldPos;
    fragLightSpacePos1 = ubo.lightSpaceMatrix1 * worldPos;
    fragLightSpacePos2 = ubo.lightSpaceMatrix2 * worldPos;
    fragViewDepth      = -viewPos.z;  // positive linear depth in view space
    fragShininess      = inParams.x;
    fragMetallic       = inParams.y;
    fragRoughness      = inParams.z;
    fragEmissive       = inParams.w;
    fragDiffuseIdx     = int(inTexIndices.x);
    fragNormalIdx      = int(inTexIndices.y);
}
