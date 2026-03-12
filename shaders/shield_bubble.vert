#version 450

// ── Glass Shield Bubble — Vertex Shader ──────────────────────────────────────
// The sphere is a procedural unit sphere; the vertex position IS the outward
// normal, which the fragment shader uses directly for Fresnel calculation.

layout(location = 0) in vec3 aPos;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vWorldNormal;

layout(push_constant) uniform PC {
    mat4  viewProj;       // 64 B
    vec3  sphereCenter;   // 12 B
    float radius;         //  4 B
    vec3  cameraPos;      // 12 B
    float time;           //  4 B
    float alpha;          //  4 B
    float _pad[3];        // 12 B
} pc;                     // 112 B total

void main() {
    vec3 worldPos    = pc.sphereCenter + aPos * pc.radius;
    vWorldPos        = worldPos;
    vWorldNormal     = aPos;        // unit sphere: vertex position == outward normal
    gl_Position      = pc.viewProj * vec4(worldPos, 1.0);
}
