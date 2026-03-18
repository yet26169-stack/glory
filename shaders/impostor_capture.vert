#version 450

layout(push_constant) uniform PC {
    mat4 viewProj;
    mat4 model;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec3 fragNormal;

void main() {
    fragColor = inColor;
    fragTexCoord = inTexCoord;
    fragNormal = mat3(pc.model) * inNormal;
    gl_Position = pc.viewProj * pc.model * vec4(inPos, 1.0);
}
