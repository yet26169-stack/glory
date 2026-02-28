#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(binding = 1) uniform sampler2D textures[64]; // bindless texture array

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
} lightData;

layout(binding = 3) uniform sampler2D shadowMap;

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

layout(location = 0) out vec4 outColor;

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

float calcShadow(vec4 lsPos) {
    vec3 proj = lsPos.xyz / lsPos.w;
    proj.xy = proj.xy * 0.5 + 0.5;
    if (proj.x < 0.0 || proj.x > 1.0 ||
        proj.y < 0.0 || proj.y > 1.0 ||
        proj.z < 0.0 || proj.z > 1.0)
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

    float shadow = 0.0;
    vec2 texelSize = 1.0 / textureSize(shadowMap, 0);
    float bias = 0.003;
    float spread = 2.5; // penumbra radius in texels

    for (int i = 0; i < 16; ++i) {
        float depth = texture(shadowMap, proj.xy + poissonDisk[i] * texelSize * spread).r;
        shadow += (proj.z - bias > depth) ? 0.0 : 1.0;
    }
    shadow /= 16.0;

    // Fade shadows at shadow map boundaries for smooth falloff
    float edgeFade = 1.0;
    vec2 edgeDist = min(proj.xy, 1.0 - proj.xy);
    float minEdge = min(edgeDist.x, edgeDist.y);
    edgeFade = smoothstep(0.0, 0.05, minEdge);
    shadow = mix(1.0, shadow, edgeFade);

    return shadow;
}

// ── PBR: Cook-Torrance BRDF components ──────────────────────────────────────

// GGX/Trowbridge-Reitz normal distribution
float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

// Schlick-GGX geometry function
float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

// Smith's method combining view and light geometry terms
float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

// Fresnel-Schlick approximation
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    // Bindless texture lookup using per-instance indices
    vec3 albedo = texture(textures[nonuniformEXT(fragDiffuseIdx)], fragTexCoord).rgb * fragColor;
    vec3 N      = normalize(fragWorldNormal);

    // Normal map via cotangent-frame TBN (bindless lookup)
    vec3 mapN = texture(textures[nonuniformEXT(fragNormalIdx)], fragTexCoord).rgb * 2.0 - 1.0;
    mat3 TBN  = cotangentFrame(N, fragWorldPos, fragTexCoord);
    N         = normalize(TBN * mapN);

    vec3 V = normalize(lightData.viewPos - fragWorldPos);

    // PBR material params: negative metallic = use legacy Blinn-Phong path
    float metallic  = fragMetallic;
    float roughness = fragRoughness;

    // Clamp roughness to avoid divide-by-zero
    roughness = clamp(roughness, 0.04, 1.0);

    // Base reflectivity: dielectrics ~0.04, metals use albedo
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    float shadow = calcShadow(fragLightSpacePos);

    vec3 Lo = vec3(0.0);

    for (int i = 0; i < lightData.lightCount; ++i) {
        vec3  L    = normalize(lightData.lights[i].position - fragWorldPos);
        vec3  H    = normalize(V + L);
        float dist = length(lightData.lights[i].position - fragWorldPos);
        float atten = 1.0 / (1.0 + 0.09 * dist + 0.032 * dist * dist);
        vec3  radiance = lightData.lights[i].color * atten;

        // Cook-Torrance BRDF
        float NDF = distributionGGX(N, H, roughness);
        float G   = geometrySmith(N, V, L, roughness);
        vec3  F   = fresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3  numerator   = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
        vec3  specular    = numerator / denominator;

        // Energy conservation: diffuse is what's not reflected
        vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

        float NdotL = max(dot(N, L), 0.0);

        // Shadow only on primary light (index 0)
        float lightShadow = (i == 0) ? shadow : 1.0;

        Lo += (kD * albedo / PI + specular) * radiance * NdotL * lightShadow;

        // ── Subsurface scattering approximation (wrap lighting) ─────────
        // For non-metallic, smooth surfaces: light wraps around the surface
        if (metallic < 0.3 && roughness < 0.5) {
            float wrap = 0.5;
            float NdotL_wrap = (dot(N, L) + wrap) / (1.0 + wrap);
            NdotL_wrap = max(NdotL_wrap, 0.0);
            float sssStrength = (1.0 - metallic) * (1.0 - roughness * 2.0) * 0.25;
            vec3 sssColor = albedo * vec3(1.0, 0.4, 0.2); // warm back-scatter tint
            Lo += sssColor * radiance * NdotL_wrap * sssStrength * lightShadow;
        }
    }

    // ── Indirect lighting (analytic IBL) ────────────────────────────────────
    // Fresnel at grazing angle (roughness-attenuated)
    vec3 F_ibl = F0 + (max(vec3(1.0 - roughness), F0) - F0) *
                 pow(clamp(1.0 - max(dot(N, V), 0.0), 0.0, 1.0), 5.0);

    // Analytic sky color from reflection direction (matches sky.frag gradient)
    vec3 R = reflect(-V, N);
    float skyT = clamp(R.y * 0.5 + 0.5, 0.0, 1.0); // map y[-1,1] to [0,1]
    vec3 skyHorizon = vec3(0.45, 0.55, 0.70);
    vec3 skyZenith  = vec3(0.05, 0.08, 0.20);
    vec3 envColor   = mix(skyHorizon, skyZenith, skyT * skyT);

    // Roughness blurs reflections: blend toward diffuse irradiance
    vec3 diffuseIrr = mix(skyHorizon, skyZenith, 0.25) * 0.8; // average sky color
    vec3 specEnv = mix(envColor, diffuseIrr, roughness * roughness);

    // Hemisphere ambient: blend sky/ground based on world normal Y
    float hemiT = N.y * 0.5 + 0.5;
    vec3 groundColor = vec3(0.15, 0.12, 0.10); // warm brown ground bounce
    vec3 hemiAmbient = mix(groundColor, diffuseIrr, hemiT);

    // Combine indirect: diffuse (kD * albedo * irradiance) + specular (F * env)
    vec3 kD_ibl = (vec3(1.0) - F_ibl) * (1.0 - metallic);
    vec3 ambient = kD_ibl * albedo * hemiAmbient * lightData.ambientStrength
                 + F_ibl * specEnv * lightData.ambientStrength;

    // ── Fresnel rim lighting ────────────────────────────────────────────────
    float NdotV_rim = max(dot(N, V), 0.0);
    float rim = pow(1.0 - NdotV_rim, 4.0);
    vec3 rimColor = mix(vec3(0.3, 0.4, 0.6), envColor, 0.5); // sky-tinted rim
    vec3 rimLight = rim * rimColor * 0.3 * (1.0 - roughness); // smooth = more rim

    // ── Thin-film iridescence on metallic surfaces ──────────────────────────
    vec3 iridescentColor = vec3(0.0);
    if (metallic > 0.3) {
        float cosAngle = NdotV_rim;
        // Approximate thin-film interference: shift hue based on view angle
        float phase = cosAngle * 6.0 + 1.0;
        iridescentColor.r = 0.5 + 0.5 * sin(phase);
        iridescentColor.g = 0.5 + 0.5 * sin(phase + 2.094); // +2π/3
        iridescentColor.b = 0.5 + 0.5 * sin(phase + 4.189); // +4π/3
        float iriStrength = metallic * rim * 0.15; // subtle, edge-only
        iridescentColor *= iriStrength;
    }

    vec3 result = ambient + Lo + rimLight + iridescentColor;

    // ── Emissive glow ───────────────────────────────────────────────────────
    if (fragEmissive > 0.0) {
        result += albedo * fragEmissive;
    }

    // ── Micro ambient occlusion (curvature-based) ───────────────────────────
    vec3 dNdx = dFdx(N);
    vec3 dNdy = dFdy(N);
    float curvature = length(dNdx) + length(dNdy);
    float microAO = 1.0 - clamp(curvature * 3.0, 0.0, 0.4);
    result *= microAO;

    // Exponential distance fog
    float fogDist = length(lightData.viewPos - fragWorldPos);
    float fogFactor = exp(-lightData.fogDensity * max(fogDist - lightData.fogStart, 0.0));
    fogFactor = clamp(fogFactor, 0.0, 1.0);
    result = mix(lightData.fogColor, result, fogFactor);

    outColor = vec4(result, 1.0);
}
