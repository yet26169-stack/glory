#version 450

layout(binding = 0) uniform sampler2D aoTex;

layout(location = 0) in  vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 texelSize = 1.0 / textureSize(aoTex, 0);
    float result = 0.0;

    // 4x4 box blur
    for (int x = -2; x < 2; ++x) {
        for (int y = -2; y < 2; ++y) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            result += texture(aoTex, fragTexCoord + offset).r;
        }
    }

    result /= 16.0;
    outColor = vec4(result, result, result, 1.0);
}
