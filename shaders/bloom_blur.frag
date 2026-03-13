#version 450

layout(set = 0, binding = 0) uniform sampler2D inputColor;

layout(push_constant) uniform BloomPC {
    uint horizontal;
    float threshold;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

const float weight[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

void main() {
    vec2 tex_offset = 1.0 / textureSize(inputColor, 0);
    vec3 result = texture(inputColor, uv).rgb * weight[0];
    
    if (pc.horizontal != 0) {
        for (int i = 1; i < 5; ++i) {
            result += texture(inputColor, uv + vec2(tex_offset.x * i, 0.0)).rgb * weight[i];
            result += texture(inputColor, uv - vec2(tex_offset.x * i, 0.0)).rgb * weight[i];
        }
    } else {
        for (int i = 1; i < 5; ++i) {
            result += texture(inputColor, uv + vec2(0.0, tex_offset.y * i)).rgb * weight[i];
            result += texture(inputColor, uv - vec2(0.0, tex_offset.y * i)).rgb * weight[i];
        }
    }
    
    outColor = vec4(result, 1.0);
}
