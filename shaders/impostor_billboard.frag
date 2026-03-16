#version 450

// Impostor billboard fragment shader.
// Samples the impostor atlas and applies per-instance tint.

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragTint;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D atlasTexture;

void main() {
    vec4 texel = texture(atlasTexture, fragUV);

    // Discard fully transparent pixels (alpha-tested billboards)
    if (texel.a < 0.1) discard;

    outColor = texel * fragTint;
}
