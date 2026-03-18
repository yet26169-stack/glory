#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragNormal;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 N = normalize(fragNormal);
    vec3 L = normalize(vec3(0.5, 1.0, 0.2));
    float dotNL = max(dot(N, L), 0.0);
    
    vec3 ambient = vec3(0.3);
    vec3 diffuse = vec3(dotNL);
    
    vec3 final = (ambient + diffuse) * fragColor;
    outColor = vec4(final, 1.0);
}
