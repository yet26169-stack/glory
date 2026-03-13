#version 450

// ── Cone Ability — Wave Shockwave Fragment Shader ─────────────────────────────
// A glowing energy ring (the wave front) sweeps from apex to far edge over
// WAVE_DUR seconds, leaving a fading energy trail. After the ring reaches
// the edge the whole effect fades out over FADE_DUR seconds.

layout(location = 0) in vec2  fragUV;    // u_angle ∈[0,1], v_radius ∈[0,1]
layout(location = 1) in vec3  fragWorldPos;
layout(location = 2) in float fragEdge;  // 0 = axis center, 1 = side edges

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform ConePC {
    mat4  viewProj;      // 64 B
    vec3  apex;          // 12 B
    float time;          //  4 B
    vec3  axisDir;       // 12 B
    float halfAngleTan;  //  4 B
    vec3  cameraPos;     // 12 B
    float range;         //  4 B
    float alpha;         //  4 B
    float elapsed;       //  4 B
    float phase;         //  4 B
    float pad[1];        //  4 B
} pc;                    // 128 B total

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i),           hash(i+vec2(1,0)), f.x),
               mix(hash(i+vec2(0,1)), hash(i+vec2(1,1)), f.x), f.y);
}

void main() {
    float u = fragUV.x;   // 0..1 angle fraction
    float v = fragUV.y;   // 0..1 radius fraction (0=apex tip, 1=far arc)

    const float WAVE_DUR = 0.6;   // seconds for front to travel apex→edge
    const float FADE_DUR = 0.25;  // seconds to fade out after front reaches edge

    float wavePos  = clamp(pc.elapsed / WAVE_DUR, 0.0, 1.0);   // 0→1
    float postFade = 1.0 - clamp((pc.elapsed - WAVE_DUR) / FADE_DUR, 0.0, 1.0);

    // ── Wave ring ─────────────────────────────────────────────────────────────
    // Leading hard edge + wider soft tail
    float dist      = v - wavePos;
    float ring      = exp(-pow(dist / 0.07, 2.0)) * 2.0;    // sharp bright ring
    float tail      = exp(-pow(max(dist, 0.0) / 0.22, 2.0)) * 0.5; // soft forward glow
    // Additional soft wake behind the ring
    float wake      = (v < wavePos) ? exp(-pow((wavePos - v) / 0.35, 2.0)) * 0.6 : 0.0;

    // ── Noise texture in the wake ─────────────────────────────────────────────
    float inWake = step(v, wavePos + 0.05);
    vec2  nUV    = vec2(u * 4.0, v * 3.0 - pc.time * 0.7);
    float n      = noise(nUV) * 0.5 + noise(nUV * 2.1 + 0.8) * 0.5;
    float noiseFill = inWake * n * (1.0 - smoothstep(0.0, wavePos + 0.01, v)) * 0.45;

    // ── Bright side-edge boundary glow ────────────────────────────────────────
    float edgeGlow = pow(clamp((fragEdge - 0.78) / 0.22, 0.0, 1.0), 1.5) * 1.4 * inWake;

    // ── Fade masks ────────────────────────────────────────────────────────────
    float tipFade  = smoothstep(0.0, 0.07, v);
    float sideFade = 1.0 - smoothstep(0.76, 1.0, fragEdge);

    // ── Combine ───────────────────────────────────────────────────────────────
    float wave  = (ring + tail + wake) * sideFade * tipFade;
    float fill  = (noiseFill + edgeGlow) * tipFade;
    float total = (wave + fill) * postFade;

    // ── Colour ────────────────────────────────────────────────────────────────
    // Ring front: bright cyan/white. Wake: green. Edge outline: lime green.
    vec3 ringCol  = mix(vec3(0.25, 1.0, 0.65), vec3(1.0, 1.0, 1.0), ring / 2.0);
    vec3 wakeCol  = mix(vec3(0.03, 0.75, 0.3), vec3(0.15, 0.95, 0.5), n);
    vec3 edgeCol  = vec3(0.1, 1.0, 0.4);

    float ringW   = clamp((ring + tail) / (total + 0.001), 0.0, 1.0);
    float edgeW   = clamp(edgeGlow     / (total + 0.001), 0.0, 1.0);
    vec3 col = mix(mix(wakeCol, ringCol, ringW), edgeCol, edgeW * 0.6);

    float a = total * pc.alpha;
    outColor = vec4(col * a, a);
}
