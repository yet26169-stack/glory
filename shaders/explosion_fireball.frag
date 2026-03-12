#version 450

// ── Explosion Green Plasma Core — Fragment Shader ─────────────────────────────
// Renders the central green energy sphere. Matches explosion_e.mp4:
// bright green chaos/fractal core with cyan outer glow.

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform ExplosionPC {
    mat4  viewProj;
    vec3  center;
    float elapsed;
    vec3  cameraPos;
    float maxRadius;
    float alpha;
    float appTime;
    float pad[2];
} pc;

// ── 2D value noise ────────────────────────────────────────────────────────────
float hash(vec2 p) {
    p = fract(p * vec2(234.31, 456.47));
    p += dot(p, p + 34.67);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i+vec2(1,0)), u.x),
               mix(hash(i+vec2(0,1)), hash(i+vec2(1,1)), u.x), u.y);
}
float fbm(vec2 p) {
    float v=0.0, a=0.5;
    for (int i=0; i<5; ++i) { v += a*vnoise(p); p*=2.1; a*=0.5; }
    return v;
}

void main() {
    float age = pc.elapsed;

    // Animated noise coordinates — swirling green chaos
    vec2 noiseUV = fragWorldPos.xz * 0.7 + vec2(pc.appTime * 0.4, pc.appTime * -0.3);
    float n1 = fbm(noiseUV);
    float n2 = fbm(noiseUV * 1.5 + vec2(n1 * 0.8));  // domain-warped FBM

    // Fresnel for edge glow
    vec3 V = normalize(pc.cameraPos - fragWorldPos);
    float NdotV  = max(dot(normalize(fragNormal), V), 0.0);
    float fresnel = pow(1.0 - NdotV, 3.0);

    // ── Green plasma color ramp ─────────────────────────────────────────────
    float heat = clamp(n2 * 1.6 - age * 0.25, 0.0, 1.0);

    vec3 darkGreen   = vec3(0.0,  0.5,  0.15);  // deep green
    vec3 brightGreen = vec3(0.15, 1.0,  0.35);  // neon green
    vec3 cyanWhite   = vec3(0.6,  1.0,  0.85);  // hot near-white

    vec3 coreColor = mix(darkGreen,
                         mix(brightGreen, cyanWhite, clamp(heat * 2.0 - 0.5, 0.0, 1.0)),
                         heat);

    // Cyan outer glow from Fresnel (rim of the sphere glows cyan/teal)
    vec3 rimColor = vec3(0.0, 0.85, 1.0);
    coreColor = mix(coreColor, rimColor, fresnel * 0.75);

    // ── Opacity timeline ──────────────────────────────────────────────────
    float fadeIn   = clamp(age / 0.08, 0.0, 1.0);
    float fadeOut  = 1.0 - clamp((age - 1.0) / 0.8, 0.0, 1.0);
    float noiseAlp = 0.65 + n2 * 0.35;

    float a = fadeIn * fadeOut * noiseAlp * pc.alpha;
    if (a < 0.008) discard;

    outColor = vec4(coreColor, a);
}
