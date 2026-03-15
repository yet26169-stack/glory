#version 450

// ── Inking pass: depth-only Sobel edge detection ─────────────────────────────
// TODO (Prompt 10): Add normal discontinuity detection for SC2-style inner-edge
// inking (arm/torso creases, etc.). Requires a world-normal G-buffer pass written
// before this shader runs. Planned: add binding 1 sampler2D normalTex, run the
// same 3×3 Sobel on .rgb, combine: float G = max(depthG, normalG * normalWeight).
// ─────────────────────────────────────────────────────────────────────────────
layout(binding = 0) uniform sampler2D depthTex;

layout(push_constant) uniform PC {
    vec4  inkColor;
    float threshold;
    float thickness;
} pc;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 texelSize = pc.thickness / vec2(textureSize(depthTex, 0));

    float tl = texture(depthTex, fragUV + texelSize * vec2(-1.0, -1.0)).r;
    float t  = texture(depthTex, fragUV + texelSize * vec2( 0.0, -1.0)).r;
    float tr = texture(depthTex, fragUV + texelSize * vec2( 1.0, -1.0)).r;
    float ml = texture(depthTex, fragUV + texelSize * vec2(-1.0,  0.0)).r;
    float mr = texture(depthTex, fragUV + texelSize * vec2( 1.0,  0.0)).r;
    float bl = texture(depthTex, fragUV + texelSize * vec2(-1.0,  1.0)).r;
    float b  = texture(depthTex, fragUV + texelSize * vec2( 0.0,  1.0)).r;
    float br = texture(depthTex, fragUV + texelSize * vec2( 1.0,  1.0)).r;

    float Gx = -tl - 2.0*ml - bl + tr + 2.0*mr + br;
    float Gy = -tl - 2.0*t  - tr + bl + 2.0*b  + br;
    float G  = sqrt(Gx*Gx + Gy*Gy);

    if (G <= pc.threshold) discard;

    outColor = vec4(pc.inkColor.rgb, pc.inkColor.a);
}
