#version 450

// ── Explosion Energy Rings + Spikes — Fragment Shader ──────────────────────────
// Recreates the cyan shockwave rings and 8-direction cardinal spike beams from
// the explosion_e.mp4 reference. Color palette: cyan (#00CFFF) + blue-white.
//
// Disk vertex covers 2.5× maxRadius so spikes reach far past the ring edge.

layout(location = 0) in vec2 fragUV;        // (ringFrac, thetaFrac) — not used directly
layout(location = 1) in vec3 fragWorldPos;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform ExplosionPC {
    mat4  viewProj;
    vec3  center;
    float elapsed;
    vec3  cameraPos;
    float maxRadius;
    float alpha;
    float appTime;
    float pad[2];
} pc;

void main() {
    // World-space polar coords relative to explosion center
    vec2  dir2d  = fragWorldPos.xz - pc.center.xz;
    float dist   = length(dir2d);
    float nd     = dist / pc.maxRadius;           // 0..2.5 (extended disk)
    float theta  = atan(dir2d.y, dir2d.x);        // -π..π

    float age = pc.elapsed;

    // ── Concentric expanding rings ──────────────────────────────────────────
    // Three rings at different radii, all expand out over 0.9s
    float expand = clamp(age / 0.9, 0.0, 1.0);

    float r1 = expand * 1.00;  float w1 = 0.065;
    float r2 = expand * 0.72;  float w2 = 0.042;
    float r3 = expand * 0.45;  float w3 = 0.030;
    float r4 = expand * 1.30;  float w4 = 0.05;   // 4th outer ring (faint)

    float ring1 = exp(-pow((nd - r1) / w1, 2.0));
    float ring2 = exp(-pow((nd - r2) / w2, 2.0));
    float ring3 = exp(-pow((nd - r3) / w3, 2.0));
    float ring4 = exp(-pow((nd - r4) / w4, 2.0)) * 0.5;

    float rings = ring1 * 1.6 + ring2 * 1.0 + ring3 * 0.7 + ring4;

    // ── Cardinal spike beams (8 directions) ────────────────────────────────
    // abs(cos(4θ))^N gives 8 sharp lobes; high N = narrower spikes
    float spikeMask = pow(abs(cos(theta * 4.0)), 22.0);

    // Spikes follow the outer ring edge then extend past it
    // Strong near ring1 position, then radial falloff beyond
    float spikeOnRing  = ring1 * spikeMask * 4.0;
    float spikeExtend  = spikeMask * clamp(1.0 - (nd - r1) / 1.2, 0.0, 1.0)
                       * clamp(nd / 0.15, 0.0, 1.0);  // no spike at center
    float spike = max(spikeOnRing, spikeExtend * 1.8);

    // ── Initial flash (frame 0 bright burst) ──────────────────────────────
    float flash = exp(-pow(nd / 0.4, 2.0)) * clamp(1.0 - age / 0.2, 0.0, 1.0) * 3.0;

    // ── Colors ────────────────────────────────────────────────────────────
    vec3 ringColor  = vec3(0.0,  0.85, 1.0);   // pure cyan
    vec3 spikeColor = vec3(0.35, 0.95, 1.0);   // bright cyan-white
    vec3 flashColor = vec3(0.4,  1.0,  0.7);   // teal flash

    vec3 color = ringColor * rings + spikeColor * spike + flashColor * flash;

    // ── Global fade ────────────────────────────────────────────────────────
    float fadeIn  = clamp(age / 0.05, 0.0, 1.0);
    float fadeOut = 1.0 - clamp((age - 1.7) / 0.5, 0.0, 1.0);

    float a = (rings + spike * 0.65 + flash) * fadeIn * fadeOut * pc.alpha;
    if (a < 0.005) discard;

    outColor = vec4(color, clamp(a, 0.0, 1.0));
}
