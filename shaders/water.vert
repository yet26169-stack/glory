#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 lightSpaceMatrix;
    mat4 lightSpaceMatrix1;
    mat4 lightSpaceMatrix2;
    vec4 cascadeSplits;
} ubo;

layout(push_constant) uniform PC {
    mat4  model;
    float time;
    float flowSpeed;
    float distortionStrength;
    float foamScale;
    int   normalMapIdx;
    int   flowMapIdx;
    int   foamTexIdx;
    int   ssrTexIdx;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec3 fragWorldPos;
layout(location = 2) out vec4 fragClipPos;   // screen-space position for depth comparison

void main() {
    vec4 worldPos  = pc.model * vec4(inPosition, 1.0);
    fragWorldPos   = worldPos.xyz;
    fragUV         = inUV;
    gl_Position    = ubo.proj * ubo.view * worldPos;
    fragClipPos    = gl_Position;
}
