#version 450

// ── Post-process specialization constants ────────────────────────────────────
// These are set at pipeline-creation time. Off-features compile to nothing.
layout(constant_id = 0) const int ENABLE_BLOOM        = 1;
layout(constant_id = 1) const int ENABLE_SSAO         = 1;
layout(constant_id = 2) const int ENABLE_CHROMA       = 1;
layout(constant_id = 3) const int ENABLE_GOD_RAYS     = 1;
layout(constant_id = 4) const int ENABLE_DOF          = 1;
layout(constant_id = 5) const int ENABLE_AUTO_EXPOSURE= 1;
layout(constant_id = 6) const int ENABLE_HEAT         = 1;
layout(constant_id = 7) const int ENABLE_OUTLINE      = 1;
layout(constant_id = 8) const int ENABLE_SHARPEN      = 1;
layout(constant_id = 9) const int ENABLE_FILM_GRAIN   = 1;
layout(constant_id = 10) const int ENABLE_DITHER      = 1;
layout(constant_id = 11) const int ENABLE_FXAA        = 1;
layout(constant_id = 12) const int TONE_MAP_MODE      = 0; // 0=ACES 1=Reinhard 2=UC2

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
    float toneMapMode;   // kept for runtime UI but TONE_MAP_MODE spec const wins
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
vec3 acesToneMap(vec3 x) {
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
vec3 reinhardToneMap(vec3 x) { return x / (x + vec3(1.0)); }
vec3 uc2Partial(vec3 x) {
    float A=0.15, B=0.50, C=0.10, D=0.20, E=0.02, F=0.30;
    return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F)) - E/F;
}
vec3 uncharted2ToneMap(vec3 x) {
    return uc2Partial(x * 2.0) / uc2Partial(vec3(11.2));
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec2 uv = fragTexCoord;
    vec2 fromCenter = uv - 0.5;
    float edgeDist = length(fromCenter);

    // ── Scene sample (with optional chromatic aberration) ───────────────────
    vec3 hdrColor;
    if (ENABLE_CHROMA == 1 && params.chromaStrength > 0.0) {
        float chrStr = params.chromaStrength * 0.01;
        float r2 = dot(fromCenter, fromCenter);
        vec2 chrOffsetR = fromCenter * r2 * chrStr *  1.5;
        vec2 chrOffsetB = fromCenter * r2 * chrStr * -1.0;
        hdrColor.r = texture(sceneTex, uv + chrOffsetR).r;
        hdrColor.g = texture(sceneTex, uv).g;
        hdrColor.b = texture(sceneTex, uv + chrOffsetB).b;
    } else {
        hdrColor = texture(sceneTex, uv).rgb;
    }

    // ── Heat distortion ──────────────────────────────────────────────────────
    if (ENABLE_HEAT == 1 && params.heatDistortion > 0.0) {
        float depth     = texture(depthTex, uv).r;
        float depthMask = smoothstep(0.0, 0.3, depth);
        float t = hash12(gl_FragCoord.xy * 0.3) * 6.28318;
        vec2 distort;
        distort.x = sin(uv.y * 40.0 + t * 3.0) * 0.003;
        distort.y = cos(uv.x * 35.0 + t * 2.7) * 0.003;
        distort *= params.heatDistortion * depthMask;
        if (ENABLE_CHROMA == 1 && params.chromaStrength > 0.0) {
            float chrStr2 = params.chromaStrength * 0.01;
            float r2b = dot(fromCenter, fromCenter);
            vec2 cOffR = fromCenter * r2b * chrStr2 *  1.5;
            vec2 cOffB = fromCenter * r2b * chrStr2 * -1.0;
            hdrColor.r = texture(sceneTex, uv + cOffR + distort).r;
            hdrColor.g = texture(sceneTex, uv + distort).g;
            hdrColor.b = texture(sceneTex, uv + cOffB + distort).b;
        } else {
            hdrColor = texture(sceneTex, uv + distort).rgb;
        }
    }

    // ── SSAO ─────────────────────────────────────────────────────────────────
    if (ENABLE_SSAO == 1) {
        float ao = texture(ssaoTex, uv).r;
        hdrColor *= ao;
    }

    // ── Bloom ────────────────────────────────────────────────────────────────
    if (ENABLE_BLOOM == 1 && params.bloomIntensity > 0.0) {
        hdrColor += texture(bloomTex, uv).rgb * params.bloomIntensity;
    }

    // ── God rays ─────────────────────────────────────────────────────────────
    if (ENABLE_GOD_RAYS == 1 && params.godRaysStrength > 0.0) {
        vec2 lightUV = vec2(params.lightScreenX, params.lightScreenY);
        vec2 deltaUV = (uv - lightUV) * params.godRaysDensity / params.godRaysSamples;
        vec2 sampleUV = uv;
        float illumination = 0.0;
        float decayAccum   = 1.0;
        int samples = int(params.godRaysSamples);
        for (int i = 0; i < samples; ++i) {
            sampleUV -= deltaUV;
            float s = dot(texture(sceneTex, clamp(sampleUV, 0.0, 1.0)).rgb,
                          vec3(0.2126, 0.7152, 0.0722));
            illumination += s * decayAccum;
            decayAccum   *= params.godRaysDecay;
        }
        hdrColor += vec3(illumination * params.godRaysStrength / params.godRaysSamples);
    }

    // ── Depth of Field ────────────────────────────────────────────────────────
    if (ENABLE_DOF == 1 && params.dofStrength > 0.0) {
        float depth  = texture(depthTex, uv).r;
        float near   = 0.1, far = 100.0;
        float linearZ = near * far / (far - depth * (far - near));
        float coc     = clamp(abs(linearZ - params.dofFocusDist) / params.dofRange
                              * params.dofStrength, 0.0, 1.0);
        if (coc > 0.01) {
            vec2 ts = 1.0 / textureSize(sceneTex, 0);
            float blurRadius = coc * 8.0;
            vec3 blurred = vec3(0.0);
            const int S = 8;
            for (int i = 0; i < S; ++i) {
                float angle  = float(i) * 6.28318 / float(S);
                vec2  offset = vec2(cos(angle), sin(angle)) * ts * blurRadius;
                blurred += texture(sceneTex, uv + offset).rgb;
            }
            blurred /= float(S);
            if (ENABLE_SSAO == 1) blurred *= texture(ssaoTex, uv).r;
            if (ENABLE_BLOOM == 1) blurred += texture(bloomTex, uv).rgb * params.bloomIntensity;
            hdrColor = mix(hdrColor, blurred, coc);
        }
    }

    // ── Auto-exposure ─────────────────────────────────────────────────────────
    float effectiveExposure = params.exposure;
    if (ENABLE_AUTO_EXPOSURE == 1 && params.autoExposure > 0.5) {
        float logSum = 0.0;
        const int GRID = 4;
        for (int gy = 0; gy < GRID; ++gy) {
            for (int gx = 0; gx < GRID; ++gx) {
                vec2 s = vec2(float(gx)+0.5, float(gy)+0.5) / float(GRID);
                float lum = dot(texture(sceneTex, s).rgb, vec3(0.2126, 0.7152, 0.0722));
                logSum += log(max(lum, 0.001));
            }
        }
        float avgLum = exp(logSum / float(GRID * GRID));
        effectiveExposure = clamp(0.5 / max(avgLum, 0.01), 0.1, 8.0) * params.exposure;
    }

    // ── Exposure + tone mapping ───────────────────────────────────────────────
    vec3 mapped = vec3(1.0) - exp(-hdrColor * effectiveExposure);

    if (TONE_MAP_MODE == 1) {
        mapped = reinhardToneMap(mapped);
    } else if (TONE_MAP_MODE == 2) {
        mapped = uncharted2ToneMap(mapped);
    } else {
        mapped = acesToneMap(mapped);
    }

    mapped = pow(mapped, vec3(1.0 / params.gamma));

    // ── FXAA ──────────────────────────────────────────────────────────────────
    if (ENABLE_FXAA == 1 && params.fxaaEnabled > 0.5) {
        vec2 texelSize = 1.0 / textureSize(sceneTex, 0);
        float lumC = dot(mapped, vec3(0.299, 0.587, 0.114));

        vec3 nN = texture(sceneTex, uv + vec2(0.0,  texelSize.y)).rgb;
        vec3 nS = texture(sceneTex, uv - vec2(0.0,  texelSize.y)).rgb;
        vec3 nE = texture(sceneTex, uv + vec2(texelSize.x, 0.0)).rgb;
        vec3 nW = texture(sceneTex, uv - vec2(texelSize.x, 0.0)).rgb;

        float lumN = dot(nN / (nN + 1.0), vec3(0.299, 0.587, 0.114));
        float lumS = dot(nS / (nS + 1.0), vec3(0.299, 0.587, 0.114));
        float lumE = dot(nE / (nE + 1.0), vec3(0.299, 0.587, 0.114));
        float lumW = dot(nW / (nW + 1.0), vec3(0.299, 0.587, 0.114));

        float lumMin   = min(lumC, min(min(lumN, lumS), min(lumE, lumW)));
        float lumMax   = max(lumC, max(max(lumN, lumS), max(lumE, lumW)));
        float lumRange = lumMax - lumMin;

        if (lumRange > max(0.0312, lumMax * 0.125)) {
            float edgeH = abs(lumN + lumS - 2.0 * lumC);
            float edgeV = abs(lumE + lumW - 2.0 * lumC);
            bool isHoriz = edgeH > edgeV;
            vec2 blendDir = isHoriz ? vec2(0.0, texelSize.y) : vec2(texelSize.x, 0.0);
            float blend   = clamp(lumRange / lumMax, 0.0, 0.75) * 0.5;
            vec3 sA = pow(acesToneMap(vec3(1.0)-exp(-texture(sceneTex, uv+blendDir).rgb*effectiveExposure)),
                          vec3(1.0/params.gamma));
            vec3 sB = pow(acesToneMap(vec3(1.0)-exp(-texture(sceneTex, uv-blendDir).rgb*effectiveExposure)),
                          vec3(1.0/params.gamma));
            mapped = mix(mapped, (sA + sB) * 0.5, blend);
        }
    }

    // ── Sharpen ───────────────────────────────────────────────────────────────
    if (ENABLE_SHARPEN == 1 && params.sharpenStrength > 0.0) {
        vec2 ts = 1.0 / textureSize(sceneTex, 0);
        vec3 sN = pow(acesToneMap(vec3(1.0)-exp(-texture(sceneTex,uv+vec2(0.0, ts.y)).rgb*effectiveExposure)), vec3(1.0/params.gamma));
        vec3 sS = pow(acesToneMap(vec3(1.0)-exp(-texture(sceneTex,uv-vec2(0.0, ts.y)).rgb*effectiveExposure)), vec3(1.0/params.gamma));
        vec3 sE = pow(acesToneMap(vec3(1.0)-exp(-texture(sceneTex,uv+vec2(ts.x, 0.0)).rgb*effectiveExposure)), vec3(1.0/params.gamma));
        vec3 sW = pow(acesToneMap(vec3(1.0)-exp(-texture(sceneTex,uv-vec2(ts.x, 0.0)).rgb*effectiveExposure)), vec3(1.0/params.gamma));
        mapped += (mapped - (sN+sS+sE+sW)*0.25) * params.sharpenStrength;
    }

    // ── Color grading ─────────────────────────────────────────────────────────
    if (params.saturation != 1.0) {
        float lum = dot(mapped, vec3(0.2126, 0.7152, 0.0722));
        mapped = mix(vec3(lum), mapped, params.saturation);
    }
    if (abs(params.colorTemp) > 0.01) {
        mapped.r += params.colorTemp * 0.1;
        mapped.b -= params.colorTemp * 0.1;
        mapped.g += params.colorTemp * 0.02;
    }

    // ── Outlines ──────────────────────────────────────────────────────────────
    if (ENABLE_OUTLINE == 1 && params.outlineStrength > 0.0) {
        vec2 ts = 1.0 / textureSize(depthTex, 0);
        float d00=texture(depthTex,uv+vec2(-ts.x,-ts.y)).r, d10=texture(depthTex,uv+vec2(0.0,-ts.y)).r, d20=texture(depthTex,uv+vec2(ts.x,-ts.y)).r;
        float d01=texture(depthTex,uv+vec2(-ts.x, 0.0)).r,                                                d21=texture(depthTex,uv+vec2(ts.x, 0.0)).r;
        float d02=texture(depthTex,uv+vec2(-ts.x, ts.y)).r, d12=texture(depthTex,uv+vec2(0.0, ts.y)).r, d22=texture(depthTex,uv+vec2(ts.x, ts.y)).r;
        float gx = -d00-2.0*d01-d02+d20+2.0*d21+d22;
        float gy = -d00-2.0*d10-d20+d02+2.0*d12+d22;
        float edge = sqrt(gx*gx + gy*gy);
        mapped = mix(mapped, vec3(0.0),
                     smoothstep(params.outlineThreshold*0.5, params.outlineThreshold, edge)
                     * params.outlineStrength);
    }

    // ── Film grain ────────────────────────────────────────────────────────────
    if (ENABLE_FILM_GRAIN == 1 && params.filmGrain > 0.0) {
        float noise = hash12(gl_FragCoord.xy * 1.7) * 2.0 - 1.0;
        mapped += noise * params.filmGrain;
    }

    // ── Vignette ──────────────────────────────────────────────────────────────
    float vignette = smoothstep(params.vignetteRadius, params.vignetteRadius - 0.45, edgeDist);
    mapped *= mix(1.0, vignette, params.vignetteStrength);

    // ── Ordered dithering ─────────────────────────────────────────────────────
    if (ENABLE_DITHER == 1 && params.ditheringStrength > 0.0) {
        const float bayer[16] = float[16](
             0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,
            12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,
             3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,
            15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0
        );
        ivec2 fc = ivec2(gl_FragCoord.xy) % 4;
        mapped += (bayer[fc.y*4+fc.x] - 0.5) * params.ditheringStrength * (8.0/255.0);
    }

    outColor = vec4(clamp(mapped, 0.0, 1.0), 1.0);
}
