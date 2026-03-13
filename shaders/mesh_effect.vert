#version 450

// ── Mesh VFX Vertex Shader ──────────────────────────────────────────────────

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(push_constant) uniform MeshEffectPC {
    mat4  viewProj; // 0-63
    vec4  color;    // 64-79 (rgba)
    vec4  data;     // 80-95 (x=alpha, y=elapsed, z=appTime, w=scale)
    vec4  posRot;   // 96-111 (xyz=pos, w=rotation)
    vec4  dir;      // 112-127 (xyz=direction, w=unused)
} pc;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragUV;
layout(location = 3) out vec4 fragColor;
layout(location = 4) out float fragAlpha;

void main() {
    float scale = pc.data.w;
    float angle = pc.posRot.w;
    
    // Create rotation matrix for Y-axis
    float c = cos(angle);
    float s = sin(angle);
    mat3 rotY = mat3(
        c, 0, s,
        0, 1, 0,
       -s, 0, c
    );

    // If we have a direction, we might want to align the mesh to it.
    // Simplified: just apply Y rotation and scale.
    vec3 localPos = inPos * scale;
    vec3 worldPos = (rotY * localPos) + pc.posRot.xyz;

    gl_Position = pc.viewProj * vec4(worldPos, 1.0);
    
    fragWorldPos = worldPos;
    fragNormal   = rotY * inNormal;
    fragUV       = inUV;
    fragColor    = pc.color;
    fragAlpha    = pc.data.x;
}
