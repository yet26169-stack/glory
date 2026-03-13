#version 450

// ── Cone Ability — Grid Fragment Shader (wave-aware) ─────────────────────────
// Retro-grid revealed only in the wake behind the travelling wave front.
// Fades out together with the energy layer after the front reaches the edge.

layout(location = 0) in vec2  fragUV;
layout(location = 1) in vec3  fragWorldPos;
layout(location = 2) in float fragEdge;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform ConePC {
    mat4  viewProj;      // 64 B
    vec3  apex;          // 12 B
    float time;          //  4 B
    vec3  axisDir;       // 12 B
    float halfAngleTan;  //  4 B
    vec3  cameraPos;     // 12 B
    float range;         //  4 B
    float alpha;         //  4 B
    float elapsed;       //  4 B
    float phase;         //  4 B
    float pad[1];        //  4 B
} pc;                    // 128 B total

void main() {
    const float WAVE_DUR = 0.6;
    const float FADE_DUR = 0.25;

    float wavePos  = clamp(pc.elapsed / WAVE_DUR, 0.0, 1.0);
    float postFade = 1.0 - clamp((pc.elapsed - WAVE_DUR) / FADE_DUR, 0.0, 1.0);

    // Grid only visible in the wake behind the wave front
    float inWake = smoothstep(wavePos + 0.02, wavePos - 0.04, fragUV.y);
    if (inWake <= 0.0) discard;

    // Grid lines: 6 angular bands, 10 radial divisions
    vec2 gUV = vec2(fragUV.x * 6.0, fragUV.y * 10.0 - pc.time * 0.5);
    vec2 f   = fract(gUV);

    float lw    = 0.06;
    float lineU = 1.0 - smoothstep(0.0, lw, min(f.x, 1.0 - f.x));
    float lineV = 1.0 - smoothstep(0.0, lw, min(f.y, 1.0 - f.y));
    float grid  = max(lineU, lineV);

    // Fade the grid near the wave front (just revealed, still materialising)
    float wakeFade  = smoothstep(wavePos, wavePos - 0.25, fragUV.y);
    // Tip and side fades
    float axialFade = smoothstep(0.0, 0.08, fragUV.y);
    float edgeFade  = 1.0 - smoothstep(0.55, 1.0, fragEdge);

    vec3 gridColor = vec3(0.05, 0.95, 0.35);
    float a = grid * axialFade * edgeFade * inWake * wakeFade * postFade * pc.alpha * 0.65;
    outColor = vec4(gridColor * a, a);
}
