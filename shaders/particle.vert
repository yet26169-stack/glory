#version 450

// ── Billboard Particle Vertex Shader ─────────────────────────────────────────
// Reads particle state directly from the SSBO — no vertex buffer needed.
// Each particle expands to a camera-facing quad (6 vertices).
// Dead particles generate a degenerate triangle that the GPU clips away.

struct Particle {
    vec4 posLife;   // xyz = world position, w = remaining lifetime
    vec4 velAge;    // xyz = velocity,       w = age
    vec4 color;     // rgba
    vec4 params;    // x = size, y = rotation, z = atlas frame, w = active
};

layout(std430, set = 0, binding = 0) readonly buffer ParticleSSBO {
    Particle particles[];
};

layout(push_constant) uniform RenderPC {
    mat4  viewProj;     // 64 bytes
    vec4  camRight;     // camera right vector (16 bytes, w unused)
    vec4  camUp;        // camera up    vector (16 bytes, w unused)
} pc;                   // total = 96 bytes (within 128-byte push constant guarantee)

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;

// CCW quad: two triangles forming a [-0.5..0.5] square
const vec2 OFFSETS[6] = vec2[6](
    vec2(-0.5,  0.5), vec2( 0.5,  0.5), vec2(-0.5, -0.5),
    vec2(-0.5, -0.5), vec2( 0.5,  0.5), vec2( 0.5, -0.5)
);

const vec2 UVS[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
    vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0)
);

void main() {
    uint pi = uint(gl_VertexIndex) / 6u;   // particle index
    uint vi = uint(gl_VertexIndex) % 6u;   // vertex index within quad

    Particle p = particles[pi];

    if (p.params.w < 0.5) {
        // Dead particle — place behind far plane so GPU clips it harmlessly
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        fragColor   = vec4(0.0);
        fragUV      = vec2(0.0);
        return;
    }

    float size  = p.params.x;
    vec2  off   = OFFSETS[vi];

    // Expand billboard in camera space (spherical billboard)
    vec3 worldPos = p.posLife.xyz
                  + pc.camRight.xyz * off.x * size
                  + pc.camUp.xyz    * off.y * size;

    gl_Position = pc.viewProj * vec4(worldPos, 1.0);
    fragColor   = p.color;
    fragUV      = UVS[vi];
}
