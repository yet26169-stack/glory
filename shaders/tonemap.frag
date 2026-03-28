#version 450

layout(set = 0, binding = 0) uniform sampler2D hdrColor;
layout(set = 0, binding = 1) uniform sampler2D bloomColor;

layout(push_constant) uniform ToneMapPC {
    float exposure;
    float bloomStrength;
    uint  enableVignette;
    uint  enableColorGrade;
    float chromaticAberration; // UV offset strength (default 0.003)
    float desaturation;        // 0=normal, 1=full grayscale (death screen)
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
    // ── Chromatic Aberration ─────────────────────────────────────────────────
    vec3 hdr;
    if (pc.chromaticAberration > 0.0) {
        vec2 center = uv - 0.5;
        float dist  = length(center);
        vec2 dir    = center * (pc.chromaticAberration * dist);
        hdr.r = texture(hdrColor, uv + dir).r;
        hdr.g = texture(hdrColor, uv).g;
        hdr.b = texture(hdrColor, uv - dir).b;
    } else {
        hdr = texture(hdrColor, uv).rgb;
    }
    vec3 bloom = texture(bloomColor, uv).rgb;

    vec3 combined = hdr + bloom * pc.bloomStrength;
    combined *= pc.exposure;

    // Debug mode: exposure == 0 → visualise raw HDR clamped to [0,1]
    if (pc.exposure == 0.0) {
        outColor = vec4(clamp(hdr, 0.0, 1.0), 1.0);
        return;
    }

    vec3 mapped = ACESFilm(combined);

    // ── LoL/SC2 Color Grading — LINEAR space (Rec.709 luminance is valid here) ─
    if (pc.enableColorGrade != 0u) {
        float luminance = dot(mapped, vec3(0.2126, 0.7152, 0.0722));
        // Shadows push toward a very subtle cool tint (avoid heavy purple wash)
        vec3 shadowShift    = vec3(0.01, 0.005, 0.02);
        // Highlights push toward warm gold (LoL highlight palette)
        vec3 highlightShift = vec3(0.03, 0.02, 0.0);
        vec3 colorGrade = mapped + mix(shadowShift, highlightShift,
                                       smoothstep(0.2, 0.8, luminance));
        mapped = mix(mapped, colorGrade, 0.15); // 15% grade intensity
    }

    // ── Saturation Boost — LINEAR space (SC2 midtone punch, mild 12%) ─────────
    float gray = dot(mapped, vec3(0.2126, 0.7152, 0.0722));
    mapped = mix(vec3(gray), mapped, 1.05);

    // ── Vignette — LINEAR space, before gamma for consistent darkening ─────────
    if (pc.enableVignette != 0u) {
        vec2  vignetteUV   = uv - 0.5;
        float vignetteDist = length(vignetteUV);
        float vignette     = 1.0 - smoothstep(0.4, 1.1, vignetteDist) * 0.25;
        mapped *= vignette;
    }

    // ── Death desaturation — grayscale tint when player is dead ──────────────
    if (pc.desaturation > 0.0) {
        float lum = dot(mapped, vec3(0.2126, 0.7152, 0.0722));
        vec3 grayColor = vec3(lum) * vec3(0.85, 0.85, 0.92); // slight cool tint
        mapped = mix(mapped, grayColor, pc.desaturation);
    }

    // Swapchain is VK_FORMAT_B8G8R8A8_SRGB — hardware applies sRGB gamma
    // automatically, so do NOT apply manual pow(1/2.2) here (that would be
    // double gamma correction, washing out the entire scene).

    outColor = vec4(mapped, 1.0);
}
