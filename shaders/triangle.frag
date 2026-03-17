#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 1, binding = 0) uniform sampler2D textures[4096]; // global bindless array

struct PointLight {
    vec3 position;
    vec3 color;
};

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
    // ── Toon / stylized shading ──────────────────────────────────────────
    vec3  rimColor;
    float rimIntensity;
    float appTime;
    float toonRampSharpness;
    float shadowWarmth;
    float shadowBiasScale;  // normal-offset bias multiplier (default 1.5, tune to taste)
    vec3  shadowTint;
    vec2  fowMapMin;
    vec2  fowMapMax;
} lightData;

layout(binding = 3) uniform sampler2D shadowMap;
layout(binding = 5) uniform sampler2D toonRamp;    // 1D (256×1) lighting ramp
layout(binding = 6) uniform sampler2D fowTexture;  // 512×512 visibility

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in vec3 fragWorldNormal;
layout(location = 4) in vec4 fragLightSpacePos;
layout(location = 5) in float fragShininess;
layout(location = 6) in float fragMetallic;
layout(location = 7) in float fragRoughness;
layout(location = 8) in float fragEmissive;
layout(location = 9) flat in int fragDiffuseIdx;
layout(location = 10) flat in int fragNormalIdx;
layout(location = 11) in vec4 fragLightSpacePos1;
layout(location = 12) in vec4 fragLightSpacePos2;
layout(location = 13) in float fragViewDepth;

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 lightSpaceMatrix;
    mat4 lightSpaceMatrix1;
    mat4 lightSpaceMatrix2;
    vec4 cascadeSplits;  // x,y,z = cascade far distances in view space
} ubo;

layout(location = 0) out vec4 outColor;
layout(location = 1) out float outCharDepth;

const float PI = 3.14159265358979323846;

// Cotangent-frame TBN — computes tangent space from derivatives
mat3 cotangentFrame(vec3 N, vec3 p, vec2 uv) {
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(dot(T, T), dot(B, B)));
    return mat3(T * invmax, B * invmax, N);
}

// Cascaded Shadow Map sampling.
// The shadow atlas is 3*TILE wide × TILE tall, with cascades laid out horizontally.
const int CASCADE_COUNT = 3;

// Directional shadow light direction (world-space, pointing toward the light).
// Matches the C++ hardcoded value in Renderer.cpp: normalize(vec3(100, 60, 100)).
const vec3  kShadowLightDir = normalize(vec3(100.0, 60.0, 100.0));
const float kShadowMapRes   = 2048.0; // matches ShadowPass::SHADOW_MAP_SIZE

// Returns the light-view-projection matrix for the given cascade index.
mat4 getLightVP(int cascade) {
    if (cascade == 0) return ubo.lightSpaceMatrix;
    if (cascade == 1) return ubo.lightSpaceMatrix1;
    return ubo.lightSpaceMatrix2;
}

float sampleShadowCascade(vec3 worldPos, vec3 worldNormal, int cascadeIndex) {
    // ── Normal Offset Bias ────────────────────────────────────────────────────
    // Instead of subtracting a constant from the depth comparison, we shift the
    // world-space sampling point along the surface normal.  The shift is largest
    // on surfaces nearly perpendicular to the light (slopeScale→1) and zero on
    // surfaces facing the light directly (nDotL→1).
    float texelSize  = 2.0 / kShadowMapRes;
    float nDotL      = max(dot(worldNormal, kShadowLightDir), 0.0);
    float slopeScale = clamp(1.0 - nDotL, 0.0, 1.0);
    vec3  offsetPos  = worldPos + worldNormal * texelSize * lightData.shadowBiasScale * slopeScale;

    // Project the offset position into light clip space for this cascade.
    vec4 lsPos = getLightVP(cascadeIndex) * vec4(offsetPos, 1.0);
    vec3 proj  = lsPos.xyz / lsPos.w;
    proj.xy    = proj.xy * 0.5 + 0.5;

    // Map X into the correct cascade tile (0..1 → cascadeIndex/3 .. (cascadeIndex+1)/3)
    float tileU = (proj.x + float(cascadeIndex)) / float(CASCADE_COUNT);
    vec2 uv = vec2(tileU, proj.y);

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z < 0.0 || proj.z > 1.0)
        return 1.0;

    // 16-sample Poisson disc for smooth soft shadows
    const vec2 poissonDisk[16] = vec2[](
        vec2(-0.94201624, -0.39906216), vec2( 0.94558609, -0.76890725),
        vec2(-0.09418410, -0.92938870), vec2( 0.34495938,  0.29387760),
        vec2(-0.91588581,  0.45771432), vec2(-0.81544232, -0.87912464),
        vec2(-0.38277543,  0.27676845), vec2( 0.97484398,  0.75648379),
        vec2( 0.44323325, -0.97511554), vec2( 0.53742981, -0.47373420),
        vec2(-0.26496911, -0.41893023), vec2( 0.79197514,  0.19090188),
        vec2(-0.24188840,  0.99706507), vec2(-0.81409955,  0.91437590),
        vec2( 0.19984126,  0.78641367), vec2( 0.14383161, -0.14100790)
    );

    float shadow  = 0.0;
    vec2  uvTexel = 1.0 / textureSize(shadowMap, 0);
    float spread  = 1.5;

    for (int i = 0; i < 16; ++i) {
        float depth = texture(shadowMap, uv + poissonDisk[i] * uvTexel * spread).r;
        shadow += (proj.z > depth) ? 0.0 : 1.0; // no constant bias — normal offset handles acne
    }
    shadow /= 16.0;

    // Fade at cascade tile boundaries
    vec2  edgeDist = min(vec2(proj.x, proj.y), 1.0 - vec2(proj.x, proj.y));
    float minEdge  = min(edgeDist.x, edgeDist.y);
    float edgeFade = smoothstep(0.0, 0.05, minEdge);
    return mix(1.0, shadow, edgeFade);
}

float calcShadow(vec3 worldPos, vec3 worldNormal, float viewDepth) {
    // Select cascade based on view-space depth
    int cascade;
    if      (viewDepth < ubo.cascadeSplits.x) cascade = 0;
    else if (viewDepth < ubo.cascadeSplits.y) cascade = 1;
    else                                       cascade = 2;

    float shadow = sampleShadowCascade(worldPos, worldNormal, cascade);

    // Blend between cascades in the overlap region for smooth transitions
    float blendRegion = 2.0; // world units of blend overlap
    if (cascade < CASCADE_COUNT - 1) {
        float splitDist   = (cascade == 0) ? ubo.cascadeSplits.x : ubo.cascadeSplits.y;
        float blendFactor = smoothstep(splitDist - blendRegion, splitDist, viewDepth);
        if (blendFactor > 0.0) {
            float nextShadow = sampleShadowCascade(worldPos, worldNormal, cascade + 1);
            shadow = mix(shadow, nextShadow, blendFactor);
        }
    }

    return shadow;
}

void main() {
    // ── Albedo: terrain splat or single bindless texture ─────────────────────
    // Sentinel: fragMetallic < -0.5 means this draw is terrain; use splat blending.
    vec3 albedo;
    if (fragMetallic < -0.5) {
        // fragDiffuseIdx = splat control map (RGBA = layer weights 0-3)
        // fragDiffuseIdx+1 .. +4 = detail textures (RGB=color, A=height)
        vec4 splatWeights = texture(textures[nonuniformEXT(fragDiffuseIdx)], fragTexCoord);

        vec4 textureA = texture(textures[nonuniformEXT(fragDiffuseIdx + 1)], fragTexCoord * 8.0);
        vec4 textureB = texture(textures[nonuniformEXT(fragDiffuseIdx + 2)], fragTexCoord * 8.0);
        vec4 textureC = texture(textures[nonuniformEXT(fragDiffuseIdx + 3)], fragTexCoord * 8.0);
        vec4 textureD = texture(textures[nonuniformEXT(fragDiffuseIdx + 4)], fragTexCoord * 8.0);

        // Height-based blending: splat weight × height-channel value.
        // Layers whose combined weight is close to the dominant layer fade out
        // sharply, giving crisp material transitions (SC2 / Blizzard style).
        float depthA = splatWeights.r * textureA.a;
        float depthB = splatWeights.g * textureB.a;
        float depthC = splatWeights.b * textureC.a;
        float depthD = splatWeights.a * textureD.a;

        float maxDepth = max(max(depthA, depthB), max(depthC, depthD));
        const float blendSharpness = 0.2;
        depthA = max(depthA - maxDepth + blendSharpness, 0.0);
        depthB = max(depthB - maxDepth + blendSharpness, 0.0);
        depthC = max(depthC - maxDepth + blendSharpness, 0.0);
        depthD = max(depthD - maxDepth + blendSharpness, 0.0);

        float totalWeight = depthA + depthB + depthC + depthD + 0.0001;
        albedo = (textureA.rgb * depthA + textureB.rgb * depthB +
                  textureC.rgb * depthC + textureD.rgb * depthD) / totalWeight;
        albedo *= fragColor;
    } else {
        // Standard single-texture lookup
        albedo = texture(textures[nonuniformEXT(fragDiffuseIdx)], fragTexCoord).rgb * fragColor;
    }

    vec3 N = normalize(fragWorldNormal);

    // Normal map via cotangent-frame TBN (skipped in terrain mode since
    // fragNormalIdx is repurposed as the second splat map).
    vec2 duv1 = dFdx(fragTexCoord);
    vec2 duv2 = dFdy(fragTexCoord);
    float uvDeriv = dot(duv1, duv1) + dot(duv2, duv2);
    if (fragMetallic >= -0.5 && uvDeriv > 1e-12) {
        vec3 mapN = texture(textures[nonuniformEXT(fragNormalIdx)], fragTexCoord).rgb * 2.0 - 1.0;
        mat3 TBN  = cotangentFrame(N, fragWorldPos, fragTexCoord);
        N         = normalize(TBN * mapN);
    }

    vec3 V = normalize(lightData.viewPos - fragWorldPos);

    // PBR material params: negative metallic = use legacy Blinn-Phong path
    float metallic  = fragMetallic;
    float roughness = fragRoughness;

    // Clamp roughness to avoid divide-by-zero
    roughness = clamp(roughness, 0.04, 1.0);

    float shadow = calcShadow(fragWorldPos, fragWorldNormal, fragViewDepth);

    // ── Stylized (LoL/SC2) lighting model ────────────────────────────────────
    vec3 litColor = vec3(0.0);
    vec3 specular = vec3(0.0);

    for (int i = 0; i < lightData.lightCount; ++i) {
        vec3  L         = normalize(lightData.lights[i].position - fragWorldPos);
        vec3  H         = normalize(V + L);
        float dist      = length(lightData.lights[i].position - fragWorldPos);
        float atten     = 1.0 / (1.0 + 0.09 * dist + 0.032 * dist * dist);
        vec3  lightColor = lightData.lights[i].color * atten;

        float lightShadow = (i == 0) ? shadow : 1.0;

        // Half-Lambert diffuse (Valve / LoL soft shadow fill)
        float NdotL_raw  = dot(N, L);
        float halfLambert = NdotL_raw * 0.5 + 0.5;
        halfLambert       = halfLambert * halfLambert; // squared for extra softness

        // Toon ramp lookup — blend raw half-Lambert with quantised ramp
        float rampValue    = texture(toonRamp, vec2(halfLambert, 0.5)).r;
        float finalDiffuse = mix(halfLambert, rampValue, lightData.toonRampSharpness);

        // Incorporate shadow map: push finalDiffuse toward 0 in occluded regions
        // so the shadow-tint colour is shown rather than black
        float shadowedDiffuse = finalDiffuse * lightShadow;

        // Cool shadow tinting (LoL) — shadow regions shift toward a cool purple
        vec3 shadowColor = mix(albedo, lightData.shadowTint, lightData.shadowWarmth);
        litColor += mix(shadowColor, albedo * lightColor, shadowedDiffuse);

        // Simplified Blinn-Phong specular — painterly pop, metallic surfaces only
        if (metallic > 0.1) {
            float spec = pow(max(dot(N, H), 0.0), lightData.shininess * 4.0);
            spec = smoothstep(0.4, 0.6, spec) * 0.3;
            specular += vec3(spec) * lightColor * lightShadow;
        }
    }

    // ── LoL-style rim lighting (team-colour outline) ─────────────────────────
    float rimFactor = 1.0 - max(dot(N, V), 0.0);
    float rim       = smoothstep(0.3, 0.7, rimFactor);
    vec3  rimLight  = rim * lightData.rimColor * lightData.rimIntensity;

    // ── Hemisphere ambient (LoL sky-dome approximation) ──────────────────────
    float hemiT  = N.y * 0.5 + 0.5;
    vec3  ambient = mix(vec3(0.12, 0.10, 0.08), vec3(0.35, 0.38, 0.45), hemiT)
                  * lightData.ambientStrength * albedo;

    vec3 result = ambient + litColor + specular + rimLight;

    // ── Emissive with pulsing ─────────────────────────────────────────────────
    if (fragEmissive > 0.0) {
        float pulse = sin(lightData.appTime * 3.0) * 0.15 + 0.85;
        result += albedo * fragEmissive * pulse;
    }

    // ── Micro ambient occlusion (curvature-based) ───────────────────────────
    vec3 dNdx = dFdx(N);
    vec3 dNdy = dFdy(N);
    float curvature = length(dNdx) + length(dNdy);
    float microAO = 1.0 - clamp(curvature * 1.5, 0.0, 0.2);
    result *= microAO;

    // ── Fog of War (LoL/SC2 style) ──────────────────────────────────────────
    vec2 fowUV = (fragWorldPos.xz - lightData.fowMapMin) / (lightData.fowMapMax - lightData.fowMapMin);
    fowUV = clamp(fowUV, 0.0, 1.0);
    float visibility = texture(fowTexture, fowUV).r;

    // Smooth transition factors
    float fowEdge = smoothstep(0.05, 0.15, visibility);  // 0→1 as unexplored→seen
    float fowMid  = smoothstep(0.45, 0.65, visibility);  // 0→1 as seen→visible

    // Unexplored: near-black with slight blue tint
    vec3 unexploredColor = vec3(0.02, 0.02, 0.05);

    // Previously explored: desaturate 70%, darken 50%, blue-gray tint
    float lum = dot(result, vec3(0.2126, 0.7152, 0.0722));
    vec3 desatResult = mix(result, vec3(lum), 0.7) * 0.5;
    vec3 prevSeenColor = mix(desatResult, vec3(0.08, 0.09, 0.14), 0.3);

    // Blend: unexplored → previously seen → fully visible
    result = mix(unexploredColor, mix(prevSeenColor, result, fowMid), fowEdge);

    // Exponential distance fog — only apply in non-visible areas to avoid washing
    // out the LoL-style clear vision area.  Attenuate fog by FoW visibility so
    // fully-visible regions get no distance fog, while unexplored areas can still
    // have atmospheric depth.
    float fogDist   = length(lightData.viewPos - fragWorldPos);
    float fogFactor = exp(-lightData.fogDensity * max(fogDist - lightData.fogStart, 0.0));
    fogFactor = clamp(fogFactor, 0.0, 1.0);
    float fogStrength = 1.0 - visibility;  // 0 in clear areas, 1 in unexplored
    result = mix(result, mix(lightData.fogColor, result, fogFactor), fogStrength);

    outColor = vec4(result, 1.0);
    outCharDepth = gl_FragCoord.z;
}
