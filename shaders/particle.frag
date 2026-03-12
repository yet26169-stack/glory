#version 450

// ── Particle Fragment Shader ──────────────────────────────────────────────────
// Samples a shared particle atlas and multiplies by the per-particle color.
// Alpha-blended with depth test (no depth write) for correct layering.

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;

layout(set = 0, binding = 1) uniform sampler2D particleAtlas;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(particleAtlas, fragUV);
    
    // Create a soft circle mask from UVs
    vec2 p = fragUV * 2.0 - 1.0;
    float dist = length(p);
    float alphaMask = smoothstep(1.0, 0.8, dist);
    
    outColor  = tex * fragColor * vec4(1.0, 1.0, 1.0, alphaMask);

    // Early discard for nearly transparent pixels (avoids depth buffer writes)
    if (outColor.a < 0.01) discard;
}
