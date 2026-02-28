#version 450

layout(binding = 0) uniform sampler2D depthTex;
layout(binding = 1) uniform sampler2D noiseTex;

layout(push_constant) uniform SSAOPC {
    mat4  projection;
    mat4  invProjection;
    float radius;
    float bias;
    float intensity;
    float _pad;
} pc;

layout(location = 0) in  vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

// 16 hemisphere kernel samples (pre-computed, cosine-weighted distribution)
const vec3 kernel[16] = vec3[](
    vec3( 0.0381, 0.0543, 0.0140),
    vec3(-0.0487, 0.0460, 0.0695),
    vec3( 0.0530,-0.0610, 0.0492),
    vec3(-0.0294, 0.0154, 0.0867),
    vec3( 0.1050, 0.0326,-0.0319),
    vec3(-0.0740, 0.1110, 0.0541),
    vec3( 0.0252,-0.1330, 0.0660),
    vec3( 0.1420, 0.0126, 0.0912),
    vec3(-0.1610, 0.0670, 0.0462),
    vec3( 0.0590,-0.0320, 0.1820),
    vec3(-0.0440, 0.1740,-0.0810),
    vec3( 0.1950,-0.1060, 0.0330),
    vec3(-0.1280, 0.0160, 0.2100),
    vec3( 0.0740, 0.2360,-0.0580),
    vec3(-0.2210, 0.0890, 0.1340),
    vec3( 0.1640,-0.1870, 0.1620)
);

// Reconstruct view-space position from depth and UV
vec3 viewPosFromDepth(vec2 uv, float depth) {
    // NDC: xy in [-1,1], z in [0,1] for Vulkan
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 viewPos = pc.invProjection * clip;
    return viewPos.xyz / viewPos.w;
}

void main() {
    float depth = texture(depthTex, fragTexCoord).r;
    if (depth >= 1.0) {
        outColor = vec4(1.0); // sky — no occlusion
        return;
    }

    vec3 fragPos = viewPosFromDepth(fragTexCoord, depth);

    // Reconstruct normal from position derivatives
    vec3 normal = normalize(cross(dFdx(fragPos), dFdy(fragPos)));

    // Random rotation from noise texture (tiled 4x4)
    vec2 noiseScale = textureSize(depthTex, 0) / vec2(4.0);
    vec3 randomVec  = texture(noiseTex, fragTexCoord * noiseScale).xyz * 2.0 - 1.0;

    // Gram-Schmidt: create TBN from normal + random vector
    vec3 tangent   = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN       = mat3(tangent, bitangent, normal);

    float occlusion = 0.0;
    for (int i = 0; i < 16; ++i) {
        // Orient sample in hemisphere around surface normal
        vec3 samplePos = fragPos + TBN * kernel[i] * pc.radius;

        // Project sample to screen space
        vec4 offset = pc.projection * vec4(samplePos, 1.0);
        offset.xyz /= offset.w;
        offset.xy = offset.xy * 0.5 + 0.5;

        // Sample depth at projected position
        float sampleDepth = texture(depthTex, offset.xy).r;
        vec3 sampleViewPos = viewPosFromDepth(offset.xy, sampleDepth);

        // Range check: only occlude if sample is close enough
        float rangeCheck = smoothstep(0.0, 1.0, pc.radius / abs(fragPos.z - sampleViewPos.z));
        occlusion += (sampleViewPos.z >= samplePos.z + pc.bias ? 1.0 : 0.0) * rangeCheck;
    }

    float ao = 1.0 - (occlusion / 16.0) * pc.intensity;
    outColor = vec4(ao, ao, ao, 1.0);
}
