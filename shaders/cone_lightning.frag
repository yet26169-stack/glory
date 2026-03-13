#version 450

// ── Cone Ability — Lightning Fragment Shader ─────────────────────────────────
// Rapid flicker (~12 Hz) that switches between yellow-white and near-invisible
// to simulate snapping electricity arcs.

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform ConePC {
    mat4  viewProj;
    vec3  apex;
    float time;
    vec3  axisDir;
    float halfAngleTan;
    vec3  cameraPos;
    float range;
    float alpha;
    float elapsed;
    float phase;
    float pad[1];
} pc;

void main() {
    // flicker independently using pc.phase
    float flicker = step(0.2, fract(pc.time * 12.0 + pc.phase));

    // Core colour: bright yellow → pure white at flicker peaks
    vec3  color = mix(vec3(1.0, 0.85, 0.15), vec3(0.95, 0.97, 1.0), flicker * 0.6);
    float a     = pc.alpha * (0.5 + flicker * 0.5);

    outColor = vec4(color, a);
}
