#version 450

// ── Upgraded Billboard Particle Vertex Shader ───────────────────────────────

struct Particle {
    vec4 posLife;   // xyz = world position, w = remaining lifetime
    vec4 velAge;    // xyz = velocity,       w = age
    vec4 color;     // rgba
    vec4 params;    // x = initialSize, y = rotation, z = angularVel, w = packed
};

layout(std430, set = 0, binding = 0) readonly buffer ParticleSSBO {
    Particle particles[];
};

struct GpuColorKey {
    vec4  color;
    float time;
    float _pad[3];
};

layout(set = 0, binding = 2) uniform EmitterUBO {
    vec4  wind_dt;        // xyz=windDir*strength, w=dt
    vec4  phys;           // x=gravity, y=drag, z=alphaCurve, w=count
    vec4  size;           // x=sizeMin, y=sizeMax, z=sizeEnd, w=reserved
    uint  colorKeyCount;
    GpuColorKey colorKeys[8];
} emitter;

layout(push_constant) uniform RenderPC {
    mat4  viewProj;     // 64 bytes
    vec4  camRight;     // camera right vector (16 bytes, w unused)
    vec4  camUp;        // camera up    vector (16 bytes, w unused)
} pc;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;

const vec2 OFFSETS[6] = vec2[6](
    vec2(-0.5,  0.5), vec2( 0.5,  0.5), vec2(-0.5, -0.5),
    vec2(-0.5, -0.5), vec2( 0.5,  0.5), vec2( 0.5, -0.5)
);

const vec2 UVS[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
    vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0)
);

void main() {
    uint pi = uint(gl_VertexIndex) / 6u;
    uint vi = uint(gl_VertexIndex) % 6u;

    Particle p = particles[pi];

    if (p.params.w < 0.1) {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        fragColor   = vec4(0.0);
        fragUV      = vec2(0.0);
        return;
    }

    float totalLife = p.posLife.w + p.velAge.w;
    float lifeFrac  = (totalLife > 0.0) ? (p.velAge.w / totalLife) : 0.0;

    // Size interpolation
    float size = p.params.x;
    if (emitter.size.z > 0.0) {
        // If sizeEnd is set, interpolate from initial size (p.params.x) to sizeEnd.
        // We calculate current size here to avoid modifying p.params.x in compute (which would break lerp).
        size = mix(p.params.x, emitter.size.z, lifeFrac);
    }

    vec2 off = OFFSETS[vi];
    
    // 2D Rotation
    float angle = p.params.y;
    float cosA = cos(angle);
    float sinA = sin(angle);
    vec2 rotated = vec2(
        off.x * cosA - off.y * sinA,
        off.x * sinA + off.y * cosA
    );

    vec3 worldPos = p.posLife.xyz
                  + pc.camRight.xyz * rotated.x * size
                  + pc.camUp.xyz    * rotated.y * size;

    gl_Position = pc.viewProj * vec4(worldPos, 1.0);
    fragColor   = p.color;
    
    // Atlas Frame UV adjustment
    uint frame = uint(p.params.w);
    // TODO: support atlas UV offsets if needed. For now assume full texture.
    fragUV = UVS[vi];
}
