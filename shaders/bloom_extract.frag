#version 450

layout(set = 0, binding = 0) uniform sampler2D hdrColor;

layout(push_constant) uniform BloomPC {
    uint horizontal;
    float threshold;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 color = texture(hdrColor, uv).rgb;
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    if (brightness > pc.threshold) {
        outColor = vec4(color, 1.0);
    } else {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
    }
}
