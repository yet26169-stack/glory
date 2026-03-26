#version 450
#extension GL_EXT_nonuniform_qualifier : require

// ── Deferred Decal Fragment Shader ──────────────────────────────────────────
// Reconstructs world position from scene depth, projects into decal-local
// space, clips to the [-0.5, 0.5]³ box, and samples the decal texture.
// Uses reversed-Z depth (near=1.0, far=0.0).

layout(set = 0, binding = 0) uniform DecalUBO {
    mat4 viewProj;
    vec2 screenSize;
    float nearPlane;
    float farPlane;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

layout(set = 1, binding = 0) uniform sampler2D textures[4096]; // bindless

layout(push_constant) uniform DecalPC {
    mat4  invDecalModel;  //  0-63
    vec4  decalColor;     // 64-79
    float opacity;        // 80-83
    float fadeDistance;    // 84-87
    uint  texIdx;         // 88-91
    float _pad;           // 92-95
} pc;

layout(location = 0) in vec4 fragClipPos;

layout(location = 0) out vec4 outColor;

void main() {
    // Screen UV from clip-space position
    vec2 screenUV = (fragClipPos.xy / fragClipPos.w) * 0.5 + 0.5;

    // Sample scene depth (reversed-Z: 1.0 = near, 0.0 = far)
    float depth = texture(sceneDepth, screenUV).r;

    // Reconstruct clip-space position from depth
    vec4 clipPos = vec4(screenUV * 2.0 - 1.0, depth, 1.0);

    // Reconstruct world position
    mat4 invViewProj = inverse(ubo.viewProj);
    vec4 worldPos4 = invViewProj * clipPos;
    vec3 worldPos = worldPos4.xyz / worldPos4.w;

    // Transform world position into decal local space [-0.5, 0.5]³
    vec3 localPos = (pc.invDecalModel * vec4(worldPos, 1.0)).xyz;

    // Clip: discard pixels outside the decal box
    if (abs(localPos.x) > 0.5 || abs(localPos.y) > 0.5 || abs(localPos.z) > 0.5)
        discard;

    // Use local XZ as decal UV (Y is the projection axis)
    vec2 decalUV = localPos.xz + 0.5;

    // Sample decal texture from bindless array
    vec4 texColor = texture(textures[nonuniformEXT(pc.texIdx)], decalUV);

    // Edge fade: smoothly fade near box boundaries
    float fadeX = smoothstep(0.5, 0.5 - pc.fadeDistance, abs(localPos.x));
    float fadeZ = smoothstep(0.5, 0.5 - pc.fadeDistance, abs(localPos.z));
    float fadeY = smoothstep(0.5, 0.5 - pc.fadeDistance, abs(localPos.y));
    float edgeFade = fadeX * fadeZ * fadeY;

    vec4 finalColor = texColor * pc.decalColor;
    finalColor.a *= pc.opacity * edgeFade;

    if (finalColor.a < 0.005) discard;

    outColor = finalColor;
}
