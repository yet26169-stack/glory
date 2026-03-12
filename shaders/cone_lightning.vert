#version 450

// ── Cone Ability — Lightning Vertex Shader ───────────────────────────────────
// Vertices are pre-positioned on the CPU in world space; just transform to clip.

layout(location = 0) in vec3 inPos;

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
    float pad[2];
} pc;

void main() {
    gl_Position = pc.viewProj * vec4(inPos, 1.0);
}
