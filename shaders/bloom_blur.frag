#version 450

layout(binding = 0) uniform sampler2D inputTex;

layout(push_constant) uniform Params {
    float dirX;
    float dirY;
} params;

layout(location = 0) in  vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 texelSize = 1.0 / textureSize(inputTex, 0);
    vec2 direction = vec2(params.dirX, params.dirY);

    // 9-tap Gaussian kernel
    float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

    vec3 result = texture(inputTex, fragTexCoord).rgb * weights[0];
    for (int i = 1; i < 5; ++i) {
        vec2 offset = direction * texelSize * float(i);
        result += texture(inputTex, fragTexCoord + offset).rgb * weights[i];
        result += texture(inputTex, fragTexCoord - offset).rgb * weights[i];
    }

    outColor = vec4(result, 1.0);
}
