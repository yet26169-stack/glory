#version 450

// ── Explosion Disk — Vertex Shader ───────────────────────────────────────────
// Decodes a flat polar vertex into a world-space disk on the ground plane.
// The disk is extended to 2.5× maxRadius so cardinal spike geometry has coverage.

layout(location = 0) in vec3 inPos;  // (cos θ, 0, sin θ)
layout(location = 1) in vec2 inUV;   // (ring fraction 0..1, theta fraction 0..1)

layout(location = 0) out vec2  fragUV;       // (ringFrac, thetaFrac)
layout(location = 1) out vec3  fragWorldPos;

layout(push_constant) uniform ExplosionPC {
    mat4  viewProj;    // 64 B
    vec3  center;      // 12 B
    float elapsed;     //  4 B
    vec3  cameraPos;   // 12 B
    float maxRadius;   //  4 B
    float alpha;       //  4 B
    float appTime;     //  4 B
    float pad[2];      //  8 B
} pc;

void main() {
    float ringFrac = inUV.x;
    // Extend disk to 2.5× maxRadius so spike geometry reaches outward
    vec3 worldPos = pc.center + vec3(inPos.x, 0.02, inPos.z) * (ringFrac * pc.maxRadius * 2.5);

    fragUV       = inUV;
    fragWorldPos = worldPos;
    gl_Position  = pc.viewProj * vec4(worldPos, 1.0);
}
