#version 450

layout(location = 0) in vec2 inUV;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec3 center;
    float size;
    int frameIndex;
    int gridCount;
    vec4 tint;
} pc;

void main() {
    vec4 texColor = texture(texSampler, inUV);
    outColor = texColor * pc.tint;
}
