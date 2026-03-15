#version 450

// ── Upgraded Particle Fragment Shader (Soft Particles) ────────────────────────

layout(set = 0, binding = 1) uniform sampler2D particleAtlas;
layout(set = 0, binding = 3) uniform sampler2D depthBuffer; // Scene depth

layout(push_constant) uniform RenderPC {
    mat4  viewProj;
    vec4  camRight;
    vec4  camUp;
    vec2  screenSize;
    float nearPlane;
    float farPlane;
} pc;

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

float linearizeDepth(float d) {
    return pc.nearPlane * pc.farPlane / (pc.farPlane - d * (pc.farPlane - pc.nearPlane));
}

void main() {
    vec4 tex = texture(particleAtlas, fragUV);
    
    // Procedural circle mask (softens atlas edges)
    float dist = length(fragUV - 0.5) * 2.0;
    float alphaMask = smoothstep(1.0, 0.8, dist);
    
    vec4 color = tex * fragColor * vec4(1.0, 1.0, 1.0, alphaMask);

    // Soft Particle Depth Fade
    vec2 screenUV = gl_FragCoord.xy / pc.screenSize;
    float sceneDepthRaw = texture(depthBuffer, screenUV).r;
    
    float sceneZ = linearizeDepth(sceneDepthRaw);
    float partZ  = linearizeDepth(gl_FragCoord.z);
    
    // Fade out over 0.5 units of depth difference
    float diff = sceneZ - partZ;
    float softFade = smoothstep(0.0, 0.5, diff);
    
    color.a *= softFade;

    if (color.a < 0.01) discard;
    outColor = color;

    // ── LoL/SC2 VFX readability boost ─────────────────────────────────────────
    // Slight gamma lift makes particles pop against the desaturated FoW world.
    outColor.rgb = pow(outColor.rgb, vec3(0.8));
    // Opaque particles get an additional 30% brightness so they glow through
    // the stylized toon lighting (SC2 approach).
    if (outColor.a > 0.3) outColor.rgb *= 1.3;
}
