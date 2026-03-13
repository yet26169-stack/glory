#version 450

// ── Ground Decal Fragment Shader ────────────────────────────────────────────

layout(set = 0, binding = 0) uniform sampler2D decalTex;

layout(push_constant) uniform DecalPC {
    mat4  viewProj;
    vec3  center;
    float radius;
    float rotation;
    float alpha;
    float elapsed;
    float appTime;
    vec4  color;
} pc;

layout(location = 0) out vec4 outColor;
layout(location = 0) in vec2 fragUV;

void main() {
    vec4 tex = texture(decalTex, fragUV);
    
    // Basic circular mask if texture is empty/fallback
    float dist = length(fragUV - 0.5) * 2.0;
    float mask = smoothstep(1.0, 0.95, dist);
    
    // Use texture alpha or procedural mask
    vec4 finalColor = tex * pc.color;
    finalColor.a *= pc.alpha * mask;

    if (finalColor.a < 0.005) discard;
    
    outColor = finalColor;
}
