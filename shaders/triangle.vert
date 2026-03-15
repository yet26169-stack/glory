#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 lightSpaceMatrix;
    mat4 lightSpaceMatrix1;
    mat4 lightSpaceMatrix2;
    vec4 cascadeSplits;
} ubo;

// Per-object data SSBO (GPU-driven indirect rendering)
struct ObjectData {
    mat4 model;
    mat4 normalMatrix;
    vec4 aabbMin;
    vec4 aabbMax;
    vec4 tint;
    vec4 params;       // x=shininess, y=metallic, z=roughness, w=emissive
    vec4 texIndices;   // x=diffuseIdx, y=normalIdx
    uint meshVertexOffset;
    uint meshIndexOffset;
    uint meshIndexCount;
    uint _pad;
};

layout(std430, binding = 7) readonly buffer SceneBuffer {
    ObjectData objects[];
};

// Per-vertex attributes (binding 0)
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;

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
    ObjectData obj = objects[gl_InstanceIndex];

    vec4 worldPos = obj.model * vec4(inPosition, 1.0);
    vec4 viewPos  = ubo.view * worldPos;
    gl_Position   = ubo.proj * viewPos;

    fragColor          = inColor * obj.tint.rgb;
    fragTexCoord       = inTexCoord;
    fragWorldPos       = worldPos.xyz;
    fragWorldNormal    = mat3(obj.normalMatrix) * inNormal;
    fragLightSpacePos  = ubo.lightSpaceMatrix  * worldPos;
    fragLightSpacePos1 = ubo.lightSpaceMatrix1 * worldPos;
    fragLightSpacePos2 = ubo.lightSpaceMatrix2 * worldPos;
    fragViewDepth      = -viewPos.z;  // positive linear depth in view space
    fragShininess      = obj.params.x;
    fragMetallic       = obj.params.y;
    fragRoughness      = obj.params.z;
    fragEmissive       = obj.params.w;
    fragDiffuseIdx     = int(obj.texIndices.x);
    fragNormalIdx      = int(obj.texIndices.y);
}
