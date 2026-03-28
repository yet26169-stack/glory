#version 450

// ── Mesh VFX Energy Fragment Shader ─────────────────────────────────────────

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragUV;
layout(location = 3) in vec4 fragColor;
layout(location = 4) in float fragAlpha;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform MeshEffectPC {
    mat4  viewProj;
    vec4  color;
    vec4  data;     // x=alpha, y=elapsed, z=appTime, w=scale
    vec4  posRot;
    vec4  dir;
} pc;

void main() {
    // Basic scrolling texture effect (procedural)
    float wave = sin(fragUV.y * 10.0 - pc.data.z * 5.0) * 0.5 + 0.5;
    
    // Fresnel / Rim effect
    // We don't have camera position in PC, but we can infer it roughly or just use UVs.
    // Let's use UVs for a generic "center glow".
    float dist = length(fragUV - 0.5) * 2.0;
    float glow = exp(-dist * 2.0);

    vec4 finalColor = fragColor;
    finalColor.rgb *= (0.8 + 0.4 * wave + glow);
    finalColor.a *= fragAlpha;

    if (finalColor.a < 0.005) discard;
    finalColor.rgb = min(finalColor.rgb, vec3(3.0));
    outColor = finalColor;
}
