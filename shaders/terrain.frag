#version 450

// ─── Inputs from Vertex Shader ───
layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec3 fragTangent;

// ─── Uniforms ───
layout(set = 0, binding = 0) uniform TerrainUBO {
    mat4 view;
    mat4 proj;
    vec4 cameraPos;
    float time;
};

// ─── Textures ───
layout(set = 1, binding = 0) uniform sampler2D texGrass;
layout(set = 1, binding = 1) uniform sampler2D texPath;
layout(set = 1, binding = 2) uniform sampler2D texStone;
layout(set = 1, binding = 3) uniform sampler2D texSplatMap;
layout(set = 1, binding = 4) uniform sampler2D texTeamMap;
layout(set = 1, binding = 5) uniform sampler2D texNormalMap;

layout(location = 0) out vec4 outColor;

// ─── Constants ───
const float TILE_SCALE    = 20.0;
const vec3  SUN_DIR       = normalize(vec3(0.4, 0.8, 0.3));
const vec3  SUN_COLOR     = vec3(1.0, 0.95, 0.85);
const vec3  AMBIENT       = vec3(0.15, 0.17, 0.22);
const vec3  BLUE_TINT     = vec3(0.4, 0.5, 0.9);
const vec3  RED_TINT      = vec3(0.9, 0.4, 0.4);
const float TEAM_TINT_STR = 0.08;
const float WATER_DARKEN  = 0.4;

void main() {
    // ─── Splat Map Sampling ───
    vec4 splat = texture(texSplatMap, fragUV);
    float totalWeight = splat.r + splat.g + splat.b;
    if (totalWeight > 0.001) {
        splat.rgb /= totalWeight;
    } else {
        splat.r = 1.0;
    }

    // ─── Tiled Texture Sampling ───
    vec2 tiledUV = fragUV * TILE_SCALE;
    vec3 grass   = texture(texGrass, tiledUV).rgb;
    vec3 path    = texture(texPath,  tiledUV).rgb;
    vec3 stone   = texture(texStone, tiledUV).rgb;

    // ─── Blend ───
    vec3 baseColor = grass * splat.r + path * splat.g + stone * splat.b;

    // ─── Water Mask (from splat alpha) ───
    float waterInfluence = splat.a;
    baseColor = mix(baseColor, baseColor * WATER_DARKEN, waterInfluence);

    // ─── Normal Mapping ───
    vec3 N = normalize(fragNormal);
    vec3 T = normalize(fragTangent);
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);
    vec3 detailNormal = texture(texNormalMap, tiledUV).rgb * 2.0 - 1.0;
    vec3 worldNormal = normalize(TBN * detailNormal);

    // ─── Lighting ───
    float NdotL = max(dot(worldNormal, SUN_DIR), 0.0);
    vec3 diffuse = SUN_COLOR * NdotL;
    vec3 lit = baseColor * (AMBIENT + diffuse);

    // ─── Team Territory Tint ───
    float teamValue = texture(texTeamMap, fragUV).r;
    vec3 teamColor = mix(BLUE_TINT, RED_TINT, teamValue);
    lit = mix(lit, teamColor, TEAM_TINT_STR);

    outColor = vec4(lit, 1.0);
}
