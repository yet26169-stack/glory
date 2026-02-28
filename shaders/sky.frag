#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    // Vertical gradient: horizon (warm grey-blue) to zenith (deep blue)
    vec3 horizon = vec3(0.45, 0.55, 0.70);
    vec3 zenith  = vec3(0.05, 0.08, 0.20);
    float t = fragTexCoord.y; // 0 = bottom (horizon), 1 = top (zenith)
    vec3 sky = mix(horizon, zenith, t * t); // quadratic for softer gradient
    outColor = vec4(sky, 1.0);
}
