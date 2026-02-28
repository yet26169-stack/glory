#version 450

layout(binding = 0) uniform LightSpaceMatrix {
    mat4 lightSpaceMatrix;
};

layout(push_constant) uniform PushConstants {
    mat4 model;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;

void main() {
    gl_Position = lightSpaceMatrix * pc.model * vec4(inPosition, 1.0);
}
