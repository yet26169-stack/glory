#version 450

layout(push_constant) uniform PushData {
    mat4 viewProj;
    float particleSize;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 fragColor;

void main() {
    gl_Position  = pc.viewProj * vec4(inPosition, 1.0);
    gl_PointSize = pc.particleSize / max(gl_Position.w, 0.01);
    fragColor    = inColor;
}
