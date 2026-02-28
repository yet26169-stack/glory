#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec3 inTangent;

layout(set = 0, binding = 0) uniform TerrainUBO {
    mat4 view;
    mat4 proj;
    vec4 cameraPos;
    float time;
};

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec2 fragUV;
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec3 fragTangent;

void main() {
    fragWorldPos = inPos;
    fragUV       = inUV;
    fragNormal   = inNormal;
    fragTangent  = inTangent;

    gl_Position = proj * view * vec4(inPos, 1.0);
}
