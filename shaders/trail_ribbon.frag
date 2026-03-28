#version 450

// ── Trail / Ribbon Fragment Shader ─────────────────────────────────────────

layout(set=0, binding=1) uniform sampler2D trailAtlas;

layout(location=0) in vec4 fragColor;
layout(location=1) in vec2 fragUV;

layout(location=0) out vec4 outColor;

void main() {
    vec4 tex = texture(trailAtlas, fragUV);
    vec4 c = fragColor * tex;

    // Fade alpha based on age (fragColor.a carries the age fraction)
    float ageFade = 1.0 - fragColor.a;
    c.a *= ageFade;

    if (c.a < 0.005) discard;
    c.rgb = min(c.rgb, vec3(4.0));
    outColor = c;
}
