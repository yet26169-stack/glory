#version 450

layout(location = 0) in vec3 inPos;   // xy = billboard offset, z unused
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec2 outUV;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec3 center;
    float size;
    int frameIndex;
    int gridCount;
    int _pad[2];
    vec4 tint;
} pc;

void main() {
    // Extract camera right and up vectors from the viewProj matrix
    vec3 right = normalize(vec3(pc.viewProj[0][0], pc.viewProj[1][0], pc.viewProj[2][0]));
    vec3 up    = normalize(vec3(pc.viewProj[0][1], pc.viewProj[1][1], pc.viewProj[2][1]));

    // Billboard: position quad to face camera, centered at world-space 'center'
    vec3 worldPos = pc.center + (inPos.x * right + inPos.y * up) * pc.size;
    gl_Position = pc.viewProj * vec4(worldPos, 1.0);

    // Atlas UV calculation (same as click_indicator)
    int row = pc.frameIndex / pc.gridCount;
    int col = pc.frameIndex % pc.gridCount;
    float step = 1.0 / float(pc.gridCount);
    outUV = vec2(
        (float(col) + inUV.x) * step,
        (float(row) + inUV.y) * step
    );
}
