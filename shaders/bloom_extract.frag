#version 450

layout(binding = 0) uniform sampler2D sceneTex;

layout(push_constant) uniform Params {
    float threshold;
} params;

layout(location = 0) in  vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 color = texture(sceneTex, fragTexCoord).rgb;
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    // Soft knee: smoothly ramp from 0 at threshold to full at 2x threshold
    float knee = clamp((brightness - params.threshold) / (params.threshold + 0.0001), 0.0, 1.0);
    outColor = vec4(color * knee, 1.0);
}
