#version 450

// ── Ground Decal Fragment Shader ────────────────────────────────────────────

layout(set = 0, binding = 0) uniform sampler2D decalTex;
layout(set = 0, binding = 1) uniform sampler2D fowTex;  // FoW visibility (512×512 R8)

layout(push_constant) uniform DecalPC {
    mat4  viewProj;  // 64B
    vec3  center;    // 12B
    float radius;    //  4B
    float rotation;  //  4B
    float alpha;     //  4B
    float elapsed;   //  4B
    float appTime;   //  4B
    vec4  color;     // 16B
    vec2  fowMapMin; //  8B
    vec2  fowMapMax; //  8B
} pc;                // Total: 128B

layout(location = 0) out vec4 outColor;
layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec3 fragWorldPos;

void main() {
    vec4 tex = texture(decalTex, fragUV);
    
    // Basic circular mask if texture is empty/fallback
    float dist = length(fragUV - 0.5) * 2.0;
    float mask = smoothstep(1.0, 0.95, dist);
    
    // Use texture alpha or procedural mask
    vec4 finalColor = tex * pc.color;
    finalColor.a *= pc.alpha * mask;

    // ── Fog of War: fade decals out in unexplored areas ─────────────────────
    vec2 fowUV = (fragWorldPos.xz - pc.fowMapMin) / (pc.fowMapMax - pc.fowMapMin);
    fowUV = clamp(fowUV, 0.0, 1.0);
    float visibility = texture(fowTex, fowUV).r;
    finalColor.a *= smoothstep(0.0, 0.15, visibility);

    if (finalColor.a < 0.005) discard;
    
    outColor = finalColor;
}
