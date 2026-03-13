#version 450

// ── Distortion / Refraction Fragment Shader ────────────────────────────────

layout(set = 0, binding = 0) uniform sampler2D sceneColor; // copy of HDR before distortion

layout(push_constant) uniform DistortionPC {
    mat4  viewProj;
    vec3  center;       // world-space center of distortion
    float radius;
    float strength;     // UV offset multiplier
    float elapsed;
    vec2  screenSize;   // viewport dimensions
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 screenUV = gl_FragCoord.xy / pc.screenSize;
    
    // Project center to NDC then to UV
    vec4 clipCenter = pc.viewProj * vec4(pc.center, 1.0);
    vec2 centerUV = (clipCenter.xy / clipCenter.w) * 0.5 + 0.5;
    // Vulkan Y is downward in NDC but we use gl_FragCoord which is bottom-left? 
    // Actually gl_FragCoord is top-left in Vulkan.
    
    vec2 toCenter = screenUV - centerUV;
    float distToCenter = length(toCenter * vec2(pc.screenSize.x / pc.screenSize.y, 1.0));

    // Radial distortion: offset UV away from center
    // Falloff based on radius
    float falloff = 1.0 - smoothstep(0.0, pc.radius * 0.1, distToCenter); // arbitrary scaling for screen space
    
    // Simple ripple wave
    float wave = sin(distToCenter * 40.0 - pc.elapsed * 10.0) * 0.5 + 0.5;

    vec2 offset = normalize(toCenter) * wave * falloff * pc.strength * 0.05;
    
    vec3 distorted = texture(sceneColor, screenUV + offset).rgb;

    // Output distorted color with slight tint or just alpha for debugging
    outColor = vec4(distorted, 1.0);
}
