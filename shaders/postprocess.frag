#version 450

layout(binding = 0) uniform sampler2D sceneTex;
layout(binding = 1) uniform sampler2D bloomTex;
layout(binding = 2) uniform sampler2D ssaoTex;
layout(binding = 3) uniform sampler2D depthTex;

layout(push_constant) uniform PostParams {
    float exposure;
    float gamma;
    float bloomIntensity;
    float bloomThreshold;
    float vignetteStrength;
    float vignetteRadius;
    float chromaStrength;
    float filmGrain;
    float toneMapMode;
    float fxaaEnabled;
    float sharpenStrength;
    float dofStrength;
    float dofFocusDist;
    float dofRange;
    float saturation;
    float colorTemp;
    float outlineStrength;
    float outlineThreshold;
    float godRaysStrength;
    float godRaysDecay;
    float lightScreenX;
    float lightScreenY;
    float godRaysDensity;
    float godRaysSamples;
    float autoExposure;
    float heatDistortion;
    float ditheringStrength;
    float pad3;
} params;

layout(location = 0) in  vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

// ── Tone mapping operators ──────────────────────────────────────────────────

// ACES filmic
vec3 acesToneMap(vec3 x) {
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Reinhard (simple luminance-based)
vec3 reinhardToneMap(vec3 x) {
    return x / (x + vec3(1.0));
}

// Uncharted 2 (John Hable's filmic)
vec3 uc2Partial(vec3 x) {
    float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
vec3 uncharted2ToneMap(vec3 x) {
    float W = 11.2;
    vec3 whiteScale = vec3(1.0) / uc2Partial(vec3(W));
    return uc2Partial(x * 2.0) * whiteScale;
}

// ── Film grain ──────────────────────────────────────────────────────────────

// Hash-based noise (deterministic per pixel, varies with screen position)
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec2 uv = fragTexCoord;
    vec2 fromCenter = uv - 0.5;
    float edgeDist = length(fromCenter);

    // Chromatic aberration (barrel distortion per channel)
    float chrStr = params.chromaStrength * 0.01;
    float r2 = dot(fromCenter, fromCenter); // squared distance from center
    vec2 chrOffsetR = fromCenter * r2 * chrStr * 1.5;  // red: strongest shift
    vec2 chrOffsetB = fromCenter * r2 * chrStr * -1.0;  // blue: opposite shift
    vec3 hdrColor;
    hdrColor.r = texture(sceneTex, uv + chrOffsetR).r;
    hdrColor.g = texture(sceneTex, uv).g;
    hdrColor.b = texture(sceneTex, uv + chrOffsetB).b;

    // ── Heat distortion (animated UV shimmer) ────────────────────────────────
    if (params.heatDistortion > 0.0) {
        // Use depth to attenuate distortion (stronger on distant objects)
        float depth = texture(depthTex, uv).r;
        float depthMask = smoothstep(0.0, 0.3, depth); // fade out near camera
        // Animated sin-based perturbation
        float t = hash12(gl_FragCoord.xy * 0.3) * 6.28318;
        vec2 distort;
        distort.x = sin(uv.y * 40.0 + t * 3.0) * 0.003;
        distort.y = cos(uv.x * 35.0 + t * 2.7) * 0.003;
        distort *= params.heatDistortion * depthMask;
        // Re-sample with distorted UVs
        hdrColor.r = texture(sceneTex, uv + chrOffsetR + distort).r;
        hdrColor.g = texture(sceneTex, uv + distort).g;
        hdrColor.b = texture(sceneTex, uv + chrOffsetB + distort).b;
    }

    // SSAO
    float ao = texture(ssaoTex, uv).r;
    hdrColor *= ao;

    // Bloom
    vec3 bloom = texture(bloomTex, uv).rgb;
    hdrColor += bloom * params.bloomIntensity;

    // ── God rays (radial light shafts) ──────────────────────────────────────
    if (params.godRaysStrength > 0.0) {
        vec2 lightUV = vec2(params.lightScreenX, params.lightScreenY);
        vec2 deltaUV = (uv - lightUV) * params.godRaysDensity / params.godRaysSamples;
        vec2 sampleUV = uv;
        float illumination = 0.0;
        float decayAccum = 1.0;
        int samples = int(params.godRaysSamples);
        for (int i = 0; i < samples; ++i) {
            sampleUV -= deltaUV;
            float s = dot(texture(sceneTex, clamp(sampleUV, 0.0, 1.0)).rgb, vec3(0.2126, 0.7152, 0.0722));
            illumination += s * decayAccum;
            decayAccum *= params.godRaysDecay;
        }
        hdrColor += vec3(illumination * params.godRaysStrength / params.godRaysSamples);
    }

    // ── Depth of Field ──────────────────────────────────────────────────────
    if (params.dofStrength > 0.0) {
        float depth = texture(depthTex, uv).r;
        // Linearize depth (reverse from Vulkan NDC [0,1])
        float near = 0.1;
        float far  = 100.0;
        float linearZ = near * far / (far - depth * (far - near));

        // Circle of confusion based on distance from focus plane
        float coc = abs(linearZ - params.dofFocusDist) / params.dofRange;
        coc = clamp(coc * params.dofStrength, 0.0, 1.0);

        if (coc > 0.01) {
            // Disc blur: 8 samples in a ring
            vec2 ts = 1.0 / textureSize(sceneTex, 0);
            float blurRadius = coc * 8.0;
            vec3 blurred = vec3(0.0);
            const int SAMPLES = 8;
            for (int i = 0; i < SAMPLES; ++i) {
                float angle = float(i) * 6.28318530718 / float(SAMPLES);
                vec2 offset = vec2(cos(angle), sin(angle)) * ts * blurRadius;
                blurred += texture(sceneTex, uv + offset).rgb;
            }
            blurred /= float(SAMPLES);
            // Apply SSAO to blurred region too
            blurred *= ao;
            blurred += bloom * params.bloomIntensity;
            hdrColor = mix(hdrColor, blurred, coc);
        }
    }

    // ── Auto-exposure (log-average luminance from sparse scene samples) ────
    float effectiveExposure = params.exposure;
    if (params.autoExposure > 0.5) {
        float logSum = 0.0;
        const int GRID = 4;
        for (int gy = 0; gy < GRID; ++gy) {
            for (int gx = 0; gx < GRID; ++gx) {
                vec2 sampleUV = vec2(float(gx) + 0.5, float(gy) + 0.5) / float(GRID);
                vec3 s = texture(sceneTex, sampleUV).rgb;
                float lum = dot(s, vec3(0.2126, 0.7152, 0.0722));
                logSum += log(max(lum, 0.001));
            }
        }
        float avgLum = exp(logSum / float(GRID * GRID));
        float autoExp = 0.5 / max(avgLum, 0.01);
        autoExp = clamp(autoExp, 0.1, 8.0);
        effectiveExposure = autoExp * params.exposure;
    }

    // Exposure
    vec3 mapped = vec3(1.0) - exp(-hdrColor * effectiveExposure);

    // Tone mapping (selectable)
    int mode = int(params.toneMapMode + 0.5);
    if (mode == 1) {
        mapped = reinhardToneMap(mapped);
    } else if (mode == 2) {
        mapped = uncharted2ToneMap(mapped);
    } else {
        mapped = acesToneMap(mapped);
    }

    // Gamma correction
    mapped = pow(mapped, vec3(1.0 / params.gamma));

    // ── FXAA (simplified luminance-based edge AA) ───────────────────────────
    if (params.fxaaEnabled > 0.5) {
        vec2 texelSize = 1.0 / textureSize(sceneTex, 0);

        // Sample luminance of neighbors (approximate from tone-mapped scene)
        float lumC = dot(mapped, vec3(0.299, 0.587, 0.114));

        // We need LDR neighbor samples — re-sample and tone map at offsets
        // For efficiency, use the already tone-mapped center and sample neighbors
        vec3 nN = texture(sceneTex, uv + vec2(0.0, texelSize.y)).rgb;
        vec3 nS = texture(sceneTex, uv - vec2(0.0, texelSize.y)).rgb;
        vec3 nE = texture(sceneTex, uv + vec2(texelSize.x, 0.0)).rgb;
        vec3 nW = texture(sceneTex, uv - vec2(texelSize.x, 0.0)).rgb;

        // Quick tone-map neighbors for luminance
        float lumN = dot(nN / (nN + 1.0), vec3(0.299, 0.587, 0.114));
        float lumS = dot(nS / (nS + 1.0), vec3(0.299, 0.587, 0.114));
        float lumE = dot(nE / (nE + 1.0), vec3(0.299, 0.587, 0.114));
        float lumW = dot(nW / (nW + 1.0), vec3(0.299, 0.587, 0.114));

        float lumMin = min(lumC, min(min(lumN, lumS), min(lumE, lumW)));
        float lumMax = max(lumC, max(max(lumN, lumS), max(lumE, lumW)));
        float lumRange = lumMax - lumMin;

        // Skip FXAA on low-contrast areas
        if (lumRange > max(0.0312, lumMax * 0.125)) {
            // Edge direction
            float edgeH = abs(lumN + lumS - 2.0 * lumC);
            float edgeV = abs(lumE + lumW - 2.0 * lumC);
            bool isHorizontal = edgeH > edgeV;

            // Blend direction: perpendicular to detected edge
            vec2 blendDir = isHorizontal ? vec2(0.0, texelSize.y) : vec2(texelSize.x, 0.0);

            // Sub-pixel blend factor
            float blend = clamp(lumRange / lumMax, 0.0, 0.75) * 0.5;

            vec3 sampleA = texture(sceneTex, uv + blendDir).rgb;
            vec3 sampleB = texture(sceneTex, uv - blendDir).rgb;
            // Tone map the samples
            sampleA = pow(acesToneMap(vec3(1.0) - exp(-sampleA * effectiveExposure)),
                         vec3(1.0 / params.gamma));
            sampleB = pow(acesToneMap(vec3(1.0) - exp(-sampleB * effectiveExposure)),
                         vec3(1.0 / params.gamma));

            mapped = mix(mapped, (sampleA + sampleB) * 0.5, blend);
        }
    }

    // ── Sharpen (unsharp mask) ──────────────────────────────────────────────
    if (params.sharpenStrength > 0.0) {
        vec2 ts = 1.0 / textureSize(sceneTex, 0);
        vec3 sN = texture(sceneTex, uv + vec2(0.0, ts.y)).rgb;
        vec3 sS = texture(sceneTex, uv - vec2(0.0, ts.y)).rgb;
        vec3 sE = texture(sceneTex, uv + vec2(ts.x, 0.0)).rgb;
        vec3 sW = texture(sceneTex, uv - vec2(ts.x, 0.0)).rgb;
        // Tone-map neighbors for LDR sharpen
        sN = pow(acesToneMap(vec3(1.0) - exp(-sN * effectiveExposure)), vec3(1.0 / params.gamma));
        sS = pow(acesToneMap(vec3(1.0) - exp(-sS * effectiveExposure)), vec3(1.0 / params.gamma));
        sE = pow(acesToneMap(vec3(1.0) - exp(-sE * effectiveExposure)), vec3(1.0 / params.gamma));
        sW = pow(acesToneMap(vec3(1.0) - exp(-sW * effectiveExposure)), vec3(1.0 / params.gamma));
        vec3 blur = (sN + sS + sE + sW) * 0.25;
        mapped += (mapped - blur) * params.sharpenStrength;
    }

    // ── Color grading ─────────────────────────────────────────────────────
    // Saturation
    if (params.saturation != 1.0) {
        float lum = dot(mapped, vec3(0.2126, 0.7152, 0.0722));
        mapped = mix(vec3(lum), mapped, params.saturation);
    }

    // Color temperature shift (warm = boost red/yellow, cool = boost blue)
    if (abs(params.colorTemp) > 0.01) {
        mapped.r += params.colorTemp * 0.1;
        mapped.b -= params.colorTemp * 0.1;
        mapped.g += params.colorTemp * 0.02; // slight green shift for warmth
    }

    // ── Screen-space outlines (Sobel on depth) ────────────────────────────
    if (params.outlineStrength > 0.0) {
        vec2 ts = 1.0 / textureSize(depthTex, 0);
        float d00 = texture(depthTex, uv + vec2(-ts.x, -ts.y)).r;
        float d10 = texture(depthTex, uv + vec2( 0.0,  -ts.y)).r;
        float d20 = texture(depthTex, uv + vec2( ts.x, -ts.y)).r;
        float d01 = texture(depthTex, uv + vec2(-ts.x,  0.0)).r;
        float d21 = texture(depthTex, uv + vec2( ts.x,  0.0)).r;
        float d02 = texture(depthTex, uv + vec2(-ts.x,  ts.y)).r;
        float d12 = texture(depthTex, uv + vec2( 0.0,   ts.y)).r;
        float d22 = texture(depthTex, uv + vec2( ts.x,  ts.y)).r;

        // Sobel horizontal and vertical
        float gx = -d00 - 2.0*d01 - d02 + d20 + 2.0*d21 + d22;
        float gy = -d00 - 2.0*d10 - d20 + d02 + 2.0*d12 + d22;
        float edge = sqrt(gx*gx + gy*gy);

        float outline = smoothstep(params.outlineThreshold * 0.5,
                                   params.outlineThreshold, edge);
        mapped = mix(mapped, vec3(0.0), outline * params.outlineStrength);
    }

    // Film grain
    if (params.filmGrain > 0.0) {
        vec2 pixelCoord = gl_FragCoord.xy;
        float noise = hash12(pixelCoord * 1.7) * 2.0 - 1.0;
        mapped += noise * params.filmGrain;
    }

    // Vignette
    float vignette = smoothstep(params.vignetteRadius, params.vignetteRadius - 0.45, edgeDist);
    mapped *= mix(1.0, vignette, params.vignetteStrength);

    // Ordered dithering (4×4 Bayer matrix) to reduce color banding
    if (params.ditheringStrength > 0.0) {
        const float bayerMatrix[16] = float[16](
             0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,
            12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,
             3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,
            15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0
        );
        ivec2 fc = ivec2(gl_FragCoord.xy) % 4;
        float dither = bayerMatrix[fc.y * 4 + fc.x] - 0.5;
        mapped += dither * params.ditheringStrength * (1.0 / 255.0) * 8.0;
    }

    outColor = vec4(clamp(mapped, 0.0, 1.0), 1.0);
}
