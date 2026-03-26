#version 450

// Radial / zoom blur — fullscreen post-process.
// Samples along the direction from each pixel toward a screen-space center
// with increasing offset, then averages.  Intensity and radial falloff
// are push-constant controlled so the pass is zero-cost when inactive.

layout(set = 0, binding = 0) uniform sampler2D sceneTex;

layout(push_constant) uniform RadialBlurPC {
    vec2  center;        // normalized [0,1] screen-space blur center
    float intensity;     // 0 = no blur, 1 = full blur
    float sampleCount;   // number of samples (8–16)
    float falloffStart;  // inner radius (normalized) where blur begins
    float maxBlurDist;   // max UV-space offset per sample (controls blur width)
    float _pad0;
    float _pad1;
} pc;

layout(location = 0) in  vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 dir = fragUV - pc.center;
    float dist = length(dir);

    // Radial falloff: no blur inside falloffStart, ramps up outside
    float falloff = smoothstep(pc.falloffStart, 1.0, dist);

    // Early-out when there would be no visible blur
    if (falloff * pc.intensity < 0.001) {
        outColor = texture(sceneTex, fragUV);
        return;
    }

    // Direction from pixel toward center (we blur *away* from center)
    vec2 blurDir = normalize(dir) * pc.maxBlurDist;

    int   N     = int(pc.sampleCount);
    float invN  = 1.0 / float(N);
    vec3  accum = vec3(0.0);
    float totalWeight = 0.0;

    for (int i = 0; i < N; ++i) {
        // Offset increases linearly: 0 at center sample, maxBlurDist at outermost
        float t = float(i) * invN;
        vec2  offset = blurDir * t;
        vec2  sampleUV = fragUV + offset;

        // Clamp to valid UV range
        sampleUV = clamp(sampleUV, vec2(0.0), vec2(1.0));

        // Weight: center samples contribute more (Gaussian-ish falloff)
        float w = 1.0 - t * 0.5;
        accum += texture(sceneTex, sampleUV).rgb * w;
        totalWeight += w;
    }

    vec3 blurred = accum / totalWeight;
    vec3 original = texture(sceneTex, fragUV).rgb;

    // Final blend: original → blurred, modulated by intensity and radial falloff
    vec3 result = mix(original, blurred, falloff * pc.intensity);
    outColor = vec4(result, 1.0);
}
