#version 450

// ── Glass Shield Bubble — Fragment Shader ────────────────────────────────────
// Produces a soap-bubble / glass-sphere look:
//   • Fresnel rim   — bright white glow at silhouette edges
//   • Glass tint    — icy blue, nearly transparent interior
//   • Specular spot — Blinn-Phong highlight from below (matches screenshot)
//   • Subtle pulse  — radius breathes via a time-driven sin wave
//
// Rendered in two passes from ShieldBubbleRenderer:
//   Pass 1 (back-face):  CULL_FRONT + standard alpha blend → inner tint
//   Pass 2 (front-face): CULL_BACK  + additive blend       → glowing Fresnel rim

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vWorldNormal;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PC {
    mat4  viewProj;
    vec3  sphereCenter;
    float radius;
    vec3  cameraPos;
    float time;
    float alpha;
    float _pad[3];
} pc;

void main() {
    // Flip normal for back-face fragments so shading is correct on inner surface
    vec3 N = normalize(gl_FrontFacing ? vWorldNormal : -vWorldNormal);
    vec3 V = normalize(pc.cameraPos - vWorldPos);

    float NdotV  = max(dot(N, V), 0.0);
    float fresnel = pow(1.0 - NdotV, 4.0);          // bright white rim at silhouette

    // Glass colour: icy blue interior, white at the rim
    vec3 glassColor = mix(vec3(0.52, 0.80, 1.00),   // interior tint
                          vec3(0.90, 0.96, 1.00),   // rim/edge tint
                          fresnel);

    // Specular highlight — light positioned below-right → bright spot at bottom
    // matching the hotspot visible in the reference screenshot
    vec3 lightDir = normalize(vec3(0.3, -0.85, 0.4));
    vec3 H        = normalize(V - lightDir);
    float spec    = pow(max(dot(N, H), 0.0), 80.0) * 0.55;

    // Subtle breathing pulse on the rim intensity
    float pulse    = 0.5 + 0.5 * sin(pc.time * 2.5);
    float rimBoost = 1.0 + pulse * 0.15;

    float baseAlpha  = 0.03;
    float totalAlpha = (baseAlpha + fresnel * 0.40 * rimBoost) * pc.alpha;

    vec3 finalColor = glassColor + vec3(spec);
    outColor = vec4(finalColor, totalAlpha);
}
