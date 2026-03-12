#version 450

// ── Explosion Sphere — Vertex Shader ─────────────────────────────────────────
// Green plasma/energy sphere. Grows fast then shrinks. No vertical rise.

layout(location = 0) in vec3 inPos;  // unit sphere vertex

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;

layout(push_constant) uniform ExplosionPC {
    mat4  viewProj;
    vec3  center;
    float elapsed;
    vec3  cameraPos;
    float maxRadius;
    float alpha;
    float appTime;
    float pad[2];
} pc;

void main() {
    // Grow to 1.6 units in 0.3s, hold until 0.9s, then shrink and fade
    float growPhase   = clamp(pc.elapsed / 0.3, 0.0, 1.0);
    float shrinkPhase = clamp((pc.elapsed - 0.9) / 0.9, 0.0, 1.0);
    float radius      = mix(0.0, 1.6, growPhase) * (1.0 - shrinkPhase * 0.9);

    // Small lift so sphere doesn't clip into ground
    vec3 center = pc.center + vec3(0.0, 0.4, 0.0);
    vec3 worldPos = center + inPos * radius;

    fragWorldPos = worldPos;
    fragNormal   = inPos;
    gl_Position  = pc.viewProj * vec4(worldPos, 1.0);
}
