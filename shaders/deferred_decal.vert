#version 450

// ── Deferred Decal Vertex Shader ────────────────────────────────────────────
// Renders a unit cube [−0.5, 0.5]³ at the decal's world transform.
// Fragment shader reconstructs world position from depth and projects into
// decal-local space to determine UV and clipping.

layout(set = 0, binding = 0) uniform DecalUBO {
    mat4 viewProj;
    vec2 screenSize;
    float nearPlane;
    float farPlane;
} ubo;

layout(push_constant) uniform DecalPC {
    mat4  invDecalModel;  //  0-63
    vec4  decalColor;     // 64-79
    float opacity;        // 80-83
    float fadeDistance;    // 84-87
    uint  texIdx;         // 88-91
    float _pad;           // 92-95
} pc;                     // Total: 96 bytes

layout(location = 0) in vec3 inPosition;

layout(location = 0) out vec4 fragClipPos;

void main() {
    mat4 decalModel = inverse(pc.invDecalModel);
    vec4 worldPos = decalModel * vec4(inPosition, 1.0);
    gl_Position = ubo.viewProj * worldPos;
    fragClipPos = gl_Position;
}
