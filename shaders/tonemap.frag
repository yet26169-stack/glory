#version 450

layout(set = 0, binding = 0) uniform sampler2D hdrColor;
layout(set = 0, binding = 1) uniform sampler2D bloomColor;

layout(push_constant) uniform ToneMapPC {
    float exposure;
    float bloomStrength;
    uint  enableVignette;
    uint  enableColorGrade;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

// ACES filmic tone mapping
vec3 ACESFilm(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(hdrColor, uv).rgb;
    vec3 bloom = texture(bloomColor, uv).rgb;

    vec3 combined = hdr + bloom * pc.bloomStrength;
    combined *= pc.exposure;

    vec3 mapped = ACESFilm(combined);

    // Gamma correction (assuming swapchain is UNORM)
    mapped = pow(mapped, vec3(1.0 / 2.2));

    // ── LoL/SC2 Color Grading (warm highlights / cool shadows split) ──────────
    if (pc.enableColorGrade != 0u) {
        float luminance = dot(mapped, vec3(0.2126, 0.7152, 0.0722));
        // Shadows push toward cool purple-blue (LoL shadow palette)
        vec3 shadowShift    = vec3(0.05, 0.0, 0.1);
        // Highlights push toward warm gold (LoL highlight palette)
        vec3 highlightShift = vec3(0.08, 0.06, 0.0);
        vec3 colorGrade = mapped + mix(shadowShift, highlightShift,
                                       smoothstep(0.2, 0.8, luminance));
        mapped = mix(mapped, colorGrade, 0.4); // 40% grade intensity
    }

    // ── Vignette (LoL center-focus dark frame) ────────────────────────────────
    if (pc.enableVignette != 0u) {
        vec2  vignetteUV   = uv - 0.5;
        float vignetteDist = length(vignetteUV);
        float vignette     = 1.0 - smoothstep(0.4, 1.1, vignetteDist) * 0.25;
        mapped *= vignette;
    }

    // ── Saturation Boost (SC2 midtone punch — always active, mild 12%) ────────
    float gray = dot(mapped, vec3(0.2126, 0.7152, 0.0722));
    mapped = mix(vec3(gray), mapped, 1.12);

    outColor = vec4(mapped, 1.0);
}
