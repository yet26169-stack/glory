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
    int _pad[2];
    vec4 tint;
} pc;

void main() {
    vec4 texColor = texture(texSampler, inUV);

    // For additive blending: output RGB weighted by alpha (pre-multiplied).
    // The pipeline uses blend factors ONE/ONE, so black pixels contribute nothing.
    outColor = vec4(texColor.rgb * texColor.a * pc.tint.rgb * pc.tint.a * 0.7, texColor.a);
}
