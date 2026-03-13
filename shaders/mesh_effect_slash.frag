#version 450

// ── Mesh VFX Slash Fragment Shader ──────────────────────────────────────────

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
    // Slash UV logic: assume U is along the arc, V is across.
    // Fade at the start and end of the slash.
    float slashMask = smoothstep(0.0, 0.1, fragUV.x) * smoothstep(1.0, 0.9, fragUV.x);
    
    // Animate a "wipe" along the arc
    float lifetime = pc.data.y; // elapsed
    // Normalized elapsed time? No, we don't have duration here.
    // Let's just use it for a simple scroll.
    float wipe = smoothstep(0.0, 0.2, fragUV.x + lifetime * 2.0);

    vec4 finalColor = fragColor;
    finalColor.a *= fragAlpha * slashMask * wipe;

    if (finalColor.a < 0.005) discard;
    outColor = finalColor;
}
