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

    // Soft knee: gradual ramp from threshold-knee to threshold+knee
    float knee = 0.1; // half-width of the soft transition zone
    float soft = brightness - pc.threshold + knee;
    soft = clamp(soft / (2.0 * knee + 0.0001), 0.0, 1.0);
    soft = soft * soft; // quadratic curve for smooth rolloff

    float contribution = max(soft, brightness - pc.threshold);
    contribution = max(contribution, 0.0) / max(brightness, 0.0001);

    outColor = vec4(color * contribution, 1.0);
}
