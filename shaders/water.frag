#version 450

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec2 fragUV;

layout(set = 0, binding = 0) uniform WaterUBO {
    mat4 view;
    mat4 proj;
    vec4 cameraPos;
    float time;
};

layout(set = 1, binding = 0) uniform sampler2D texSplatMap;
layout(set = 1, binding = 1) uniform sampler2D texWaterNormal;

layout(location = 0) out vec4 outColor;

const vec3  WATER_COLOR   = vec3(0.1, 0.2, 0.5);
const vec3  WATER_SHALLOW = vec3(0.15, 0.35, 0.5);
const float FLOW_SPEED    = 0.03;
const float WATER_ALPHA   = 0.65;

void main() {
    // Only render in river area
    float riverMask = texture(texSplatMap, fragUV).a;
    if (riverMask < 0.05) discard;

    // Scrolling UV for flow effect
    vec2 flowUV1 = fragUV * 8.0 + vec2(time * FLOW_SPEED, time * FLOW_SPEED * 0.7);
    vec2 flowUV2 = fragUV * 6.0 + vec2(-time * FLOW_SPEED * 0.5, time * FLOW_SPEED * 0.3);

    // Two scrolling normal samples for ripple
    vec3 n1 = texture(texWaterNormal, flowUV1).rgb * 2.0 - 1.0;
    vec3 n2 = texture(texWaterNormal, flowUV2).rgb * 2.0 - 1.0;
    vec3 waterNormal = normalize(n1 + n2);

    // Simple specular highlight from sun
    vec3 viewDir = normalize(cameraPos.xyz - fragWorldPos);
    vec3 sunDir  = normalize(vec3(0.4, 0.8, 0.3));
    vec3 halfDir = normalize(viewDir + sunDir);
    float spec   = pow(max(dot(waterNormal, halfDir), 0.0), 64.0);

    vec3 color = mix(WATER_COLOR, WATER_SHALLOW, riverMask) + vec3(spec * 0.5);

    outColor = vec4(color, WATER_ALPHA * riverMask);
}
