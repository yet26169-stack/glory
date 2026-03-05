#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec2 outUV;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec3 center;
    float size;
    int frameIndex;
    int gridCount; // e.g. 8 for an 8x8 atlas
} pc;

void main() {
    vec3 pos = pc.center + inPos * pc.size;
    gl_Position = pc.viewProj * vec4(pos, 1.0);
    
    // Atlas UV calculation
    int row = pc.frameIndex / pc.gridCount;
    int col = pc.frameIndex % pc.gridCount;
    
    float step = 1.0 / float(pc.gridCount);
    outUV = vec2(
        (float(col) + inUV.x) * step,
        (float(row) + inUV.y) * step
    );
}
