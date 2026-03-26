#version 450

layout(set = 0, binding = 0) uniform sampler2D sceneTex;
layout(set = 0, binding = 1) uniform sampler3D lutTex;

layout(push_constant) uniform ColorGradePC {
    float lutIntensity;  // 0.0 = no grading, 1.0 = full LUT
    float lutSize;       // e.g. 32.0 or 64.0
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 color = texture(sceneTex, uv).rgb;

    // Scale and bias UVW so texel centers align with 0.0 and 1.0 input
    float scale  = (pc.lutSize - 1.0) / pc.lutSize;
    float offset = 0.5 / pc.lutSize;
    vec3  lutUVW = color * scale + offset;

    vec3 graded = texture(lutTex, lutUVW).rgb;

    outColor = vec4(mix(color, graded, pc.lutIntensity), 1.0);
}
