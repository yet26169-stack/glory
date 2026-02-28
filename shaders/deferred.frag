#version 450

layout(binding = 0) uniform sampler2D gAlbedo;
layout(binding = 1) uniform sampler2D gNormal;
layout(binding = 2) uniform sampler2D gPosition;

layout(binding = 3) uniform LightUBO {
    vec3  lightPos;
    vec3  viewPos;
    vec3  lightColor;
    float ambientStrength;
    float specularStrength;
    float shininess;
} light;

layout(location = 0) in  vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 albedo   = texture(gAlbedo,   fragTexCoord).rgb;
    vec3 normal   = texture(gNormal,   fragTexCoord).rgb * 2.0 - 1.0;
    vec3 worldPos = texture(gPosition, fragTexCoord).rgb;
    float ao      = texture(gPosition, fragTexCoord).a;

    // Ambient
    vec3 ambient = light.ambientStrength * light.lightColor * ao;

    // Diffuse
    vec3  lightDir = normalize(light.lightPos - worldPos);
    float diff     = max(dot(normal, lightDir), 0.0);
    vec3  diffuse  = diff * light.lightColor;

    // Blinn-Phong specular
    vec3  viewDir    = normalize(light.viewPos - worldPos);
    vec3  halfwayDir = normalize(lightDir + viewDir);
    float spec       = pow(max(dot(normal, halfwayDir), 0.0), light.shininess);
    vec3  specular   = light.specularStrength * spec * light.lightColor;

    vec3 result = (ambient + diffuse + specular) * albedo;
    outColor    = vec4(result, 1.0);
}
