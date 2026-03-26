#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 1, binding = 0) uniform sampler2D textures[4096];

struct PointLight { vec3 position; vec3 color; };
layout(binding = 2) uniform LightUBO {
    PointLight lights[4];
    vec3  viewPos;
    int   lightCount;
    float ambientStrength;
    float specularStrength;
    float shininess;
    vec3  fogColor;
    float fogDensity;
    float fogStart;
    float fogEnd;
    vec3  rimColor;
    float rimIntensity;
    float appTime;
    float toonRampSharpness;
    float shadowWarmth;
    vec3  shadowTint;
    vec2  fowMapMin;
    vec2  fowMapMax;
} lightData;

layout(push_constant) uniform WaterPC {
    mat4  model;
    float time;
    float flowSpeed;
    float distortionStrength;
    float foamScale;
    int   normalMapIdx;   // water surface ripple normals
    int   flowMapIdx;     // RG = flow direction encoded as [0,1]
    int   foamTexIdx;     // foam / noise texture
    int   ssrTexIdx;      // SSR reflection texture (-1 = disabled)
} pc;

layout(location = 0) in vec2  fragUV;
layout(location = 1) in vec3  fragWorldPos;
layout(location = 2) in vec4  fragClipPos;

layout(location = 0) out vec4 outColor;
// Attachment 1 (charDepth) write mask = 0 in pipeline — no output needed here

void main() {
    // ── Flow map: decode RG [0,1] → direction [-1,1], scale by flowSpeed ─────
    vec2 flowDir = texture(textures[nonuniformEXT(pc.flowMapIdx)], fragUV).rg * 2.0 - 1.0;
    flowDir *= pc.flowSpeed;

    // ── Two-phase UV offset to prevent seam discontinuities (architecture doc) ─
    // phase0 and phase1 are offset by 0.5 so when one wraps the other is at mid-
    // point. blendWeight is a triangle wave that cross-fades between the two phases.
    float phase0      = fract(pc.time * 0.1);
    float phase1      = fract(pc.time * 0.1 + 0.5);
    float blendWeight = abs(2.0 * phase0 - 1.0);

    vec2 uv0 = fragUV + flowDir * phase0;
    vec2 uv1 = fragUV + flowDir * phase1;

    // ── Surface normal from animated normal map (tiled 4×) ───────────────────
    int waterNormalIdx = pc.normalMapIdx;
    vec3 normal0 = texture(textures[nonuniformEXT(waterNormalIdx)], uv0 * 4.0).rgb * 2.0 - 1.0;
    vec3 normal1 = texture(textures[nonuniformEXT(waterNormalIdx)], uv1 * 4.0).rgb * 2.0 - 1.0;
    // Blend phases; reorient to Y-up world (scale XZ perturbation, full Y)
    vec3 Nts = normalize(mix(normal0, normal1, blendWeight));
    vec3 waterNormal = normalize(vec3(Nts.x * pc.distortionStrength,
                                     1.0,
                                     Nts.y * pc.distortionStrength));

    // ── View + half vectors ───────────────────────────────────────────────────
    vec3 V = normalize(lightData.viewPos - fragWorldPos);

    // Fresnel: deeper/more opaque at glancing angles, shallow/transparent face-on
    float fresnel    = pow(1.0 - max(dot(waterNormal, V), 0.0), 3.0);
    vec3  deepColor  = vec3(0.02, 0.08, 0.18);   // deep navy
    vec3  shallowColor = vec3(0.05, 0.25, 0.35); // teal
    vec3  waterColor = mix(deepColor, shallowColor, fresnel);

    // ── Toon-style water specular (LoL: hard-thresholded highlight) ───────────
    vec3 waterSpec = vec3(0.0);
    for (int i = 0; i < lightData.lightCount; ++i) {
        vec3  L = normalize(lightData.lights[i].position - fragWorldPos);
        vec3  H = normalize(L + V);
        float s = pow(max(dot(waterNormal, H), 0.0), 128.0);
        // Smoothstep threshold gives the painterly "pop" specular of LoL water
        s = smoothstep(0.5, 0.7, s) * 0.8;
        waterSpec += lightData.lights[i].color * s;
    }

    // ── Foam ─────────────────────────────────────────────────────────────────
    int   foamIdx   = pc.foamTexIdx;
    float foamNoise = texture(textures[nonuniformEXT(foamIdx)], fragUV * pc.foamScale).r;
    waterColor = mix(waterColor, vec3(0.88, 0.93, 1.0), foamNoise * 0.20);

    // Ambient fill
    waterColor += vec3(0.03, 0.05, 0.09) * lightData.ambientStrength;

    // ── Screen-space reflections ─────────────────────────────────────────────
    if (pc.ssrTexIdx >= 0) {
        // Compute screen UV from clip position
        vec2 screenUV = (fragClipPos.xy / fragClipPos.w) * 0.5 + 0.5;

        // Perturb screen UV with water normal for ripple distortion on reflections
        screenUV += waterNormal.xz * 0.02;
        screenUV = clamp(screenUV, vec2(0.0), vec2(1.0));

        vec4 ssr = texture(textures[nonuniformEXT(pc.ssrTexIdx)], screenUV);

        // Fresnel-weighted blend: stronger reflections at grazing angles
        float ssrFresnel = pow(1.0 - max(dot(waterNormal, V), 0.0), 3.0);
        waterColor = mix(waterColor, ssr.rgb, ssr.a * ssrFresnel * 0.6);
    }

    float waterAlpha = mix(0.45, 0.85, fresnel); // Face-on=transparent, glancing=opaque
    outColor = vec4(waterColor + waterSpec, waterAlpha);
}
