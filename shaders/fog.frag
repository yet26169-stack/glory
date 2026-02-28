#version 450

layout(location = 0) in vec2 fragUV;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;
layout(set = 0, binding = 2) uniform sampler2D visionMap;
layout(set = 0, binding = 3) uniform sampler2D explorationMap;

layout(set = 0, binding = 4) uniform FogUBO {
    mat4  invViewProj;
    vec4  fogColorExplored;
    vec4  fogColorUnknown;
};

layout(location = 0) out vec4 outColor;

vec3 ReconstructWorldPos(vec2 uv, float depth) {
    // Vulkan NDC: UV (0,0) = top-left, depth [0,1]
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 worldPos = invViewProj * clipPos;
    return worldPos.xyz / worldPos.w;
}

void main() {
    vec4 scene = texture(sceneColor, fragUV);
    float depth = texture(sceneDepth, fragUV).r;

    // Reconstruct world XZ to sample fog maps
    vec3 worldPos = ReconstructWorldPos(fragUV, depth);
    vec2 fogUV = clamp(vec2(worldPos.x / 200.0, worldPos.z / 200.0), 0.0, 1.0);

    float visibility = texture(visionMap, fogUV).r;
    float explored   = texture(explorationMap, fogUV).r;

    // Three-state fog
    if (visibility > 0.1) {
        // Currently visible — full color with edge vignette
        float edge = smoothstep(0.1, 0.4, visibility);
        outColor = mix(vec4(scene.rgb * 0.7, scene.a), scene, edge);
    } else if (explored > 0.1) {
        // Previously explored — dimmed and desaturated
        float luma = dot(scene.rgb, vec3(0.299, 0.587, 0.114));
        vec3 gray = vec3(luma);
        vec3 dimmed = mix(gray, scene.rgb, 0.3) * 0.5;
        outColor = vec4(dimmed, 1.0);
    } else {
        // Unexplored — near black
        outColor = fogColorUnknown;
    }
}
