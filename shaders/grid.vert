#version 450

layout(push_constant) uniform GridPC {
    mat4 viewProj;
    float gridY;
} pc;

// Fullscreen grid — 6 vertices forming a large quad at y = gridY
const vec3 positions[6] = vec3[](
    vec3(-50.0, 0.0, -50.0),
    vec3( 50.0, 0.0, -50.0),
    vec3( 50.0, 0.0,  50.0),
    vec3(-50.0, 0.0, -50.0),
    vec3( 50.0, 0.0,  50.0),
    vec3(-50.0, 0.0,  50.0)
);

layout(location = 0) out vec3 nearPoint;
layout(location = 1) out vec3 farPoint;
layout(location = 2) out vec2 worldXZ;

void main() {
    vec3 p = positions[gl_VertexIndex];
    p.y = pc.gridY;
    worldXZ = p.xz;
    gl_Position = pc.viewProj * vec4(p, 1.0);
    nearPoint = p;
}
