#version 450

// ── Cone Ability — Vertex Shader (FLAT 2D GROUND INDICATOR) ──────────────────
// Decodes a compact polar vertex (u_angle, unused, v_radius) into a flat
// world-space fan on the ground plane, oriented along axisDir.

layout(location = 0) in vec3 inPos; // (u_angle ∈[0,1], 0, v_radius ∈[0,1])
layout(location = 1) in vec2 inUV;  // (u_angle, v_radius)

layout(location = 0) out vec2  fragUV;
layout(location = 1) out vec3  fragWorldPos;
layout(location = 2) out float fragEdge;   // abs angular distance from axis (0=center, 1=side)
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
    float u_angle  = inPos.x;   // 0..1 → left edge to right edge
    float v_radius = inPos.z;   // 0..1 → apex tip to far arc

    // Recover actual half-angle from the tangent stored in the push constant
    float halfAngle = atan(pc.halfAngleTan);
    float theta     = (u_angle * 2.0 - 1.0) * halfAngle;

    float r = v_radius * pc.range;

    // Flat XZ forward/right basis (no vertical component)
    vec3 fwd = normalize(vec3(pc.axisDir.x, 0.0, pc.axisDir.z));
    vec3 rt  = normalize(cross(vec3(0.0, 1.0, 0.0), fwd));

    // World position: flat on ground with tiny lift to avoid z-fighting
    vec3 worldPos;
    worldPos.x = pc.apex.x + (fwd.x * cos(theta) + rt.x * sin(theta)) * r;
    worldPos.z = pc.apex.z + (fwd.z * cos(theta) + rt.z * sin(theta)) * r;
    worldPos.y = pc.apex.y;

    fragEdge     = abs(u_angle * 2.0 - 1.0);   // 0 at axis center, 1 at side edges
    fragUV       = inUV;
    fragWorldPos = worldPos;
    gl_Position  = pc.viewProj * vec4(worldPos, 1.0);
}
