#version 450

layout(set = 0, binding = 0) uniform sampler2D hdrColor;
layout(set = 0, binding = 1) uniform sampler2D bloomColor;

layout(push_constant) uniform ToneMapPC {
    float exposure;
    float bloomStrength;
    float _pad[2];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

// ACES filmic tone mapping
vec3 ACESFilm(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(hdrColor, uv).rgb;
    vec3 bloom = texture(bloomColor, uv).rgb;

    vec3 combined = hdr + bloom * pc.bloomStrength;
    combined *= pc.exposure;

    vec3 mapped = ACESFilm(combined);

    // Gamma correction (assuming swapchain is UNORM)
    mapped = pow(mapped, vec3(1.0 / 2.2));

    outColor = vec4(mapped, 1.0);
}
