#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;

layout(set = 0, binding = 0) uniform WaterUBO {
    mat4 view;
    mat4 proj;
    vec4 cameraPos;
    float time;
};

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec2 fragUV;

void main() {
    fragWorldPos = inPos;
    fragUV       = inUV;
    gl_Position  = proj * view * vec4(inPos, 1.0);
}
