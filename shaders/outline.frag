#version 450

layout(location = 0) in  vec4 inOutlineColor;

layout(location = 0) out vec4  outColor;
layout(location = 1) out float outCharDepth;

void main() {
    outColor     = inOutlineColor;
    outCharDepth = gl_FragCoord.z;
}
