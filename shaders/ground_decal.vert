#version 450

// ── Ground Decal Vertex Shader ──────────────────────────────────────────────

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(push_constant) uniform DecalPC {
    mat4  viewProj;  // 64B
    vec3  center;    // 12B
    float radius;    //  4B
    float rotation;  //  4B
    float alpha;     //  4B
    float elapsed;   //  4B
    float appTime;   //  4B
    vec4  color;     // 16B
} pc;

layout(location = 0) out vec2 fragUV;

void main() {
    // Standard unit quad: vertices should be roughly (-1, 0, -1) to (1, 0, 1) or similar.
    // Assuming inPos is a unit quad centered at origin.
    
    float c = cos(pc.rotation);
    float s = sin(pc.rotation);
    
    // Rotate quad in XZ plane
    vec2 rotated = vec2(inPos.x * c - inPos.z * s,
                        inPos.x * s + inPos.z * c);

    // Scale to radius and place at center, slightly above ground (Y=0.02) to avoid Z-fighting
    vec3 worldPos = pc.center + vec3(rotated.x * pc.radius, 0.02, rotated.y * pc.radius);
    
    gl_Position = pc.viewProj * vec4(worldPos, 1.0);
    fragUV = inUV;
}
