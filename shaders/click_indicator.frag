#version 450

layout(location = 0) in vec2 inUV;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec3 center;
    float size;
    int frameIndex;
    int gridCount;
    int _pad[2]; // Alignment padding to match C++ struct
    vec4 tint;
} pc;

void main() {
    vec4 texColor = texture(texSampler, inUV);
    outColor = texColor * pc.tint;
    
    // Safety check: if texture is missing or blank, ensure we at least see the tint
    if (texColor.a < 0.01) {
        // discard; // Uncomment to debug if quad is rendering at all
    }
}
