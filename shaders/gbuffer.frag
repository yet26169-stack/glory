#version 450

layout(binding = 1) uniform sampler2D texSampler;

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragWorldNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec3 fragColor;

// G-Buffer outputs (MRT)
layout(location = 0) out vec4 outAlbedo;    // RGB = albedo, A = metallic
layout(location = 1) out vec4 outNormal;    // RGB = normal, A = roughness
layout(location = 2) out vec4 outPosition;  // RGB = position, A = AO

void main() {
    vec3 albedo = texture(texSampler, fragTexCoord).rgb * fragColor;
    vec3 normal = normalize(fragWorldNormal);

    outAlbedo   = vec4(albedo, 0.0);       // metallic = 0
    outNormal   = vec4(normal * 0.5 + 0.5, 0.5);  // roughness = 0.5
    outPosition = vec4(fragWorldPos, 1.0);  // AO = 1.0
}
