#version 450

// ── Particle Fragment Shader (Soft Particles + Flipbook via Vertex Shader) ────

layout(set = 0, binding = 1) uniform sampler2D particleAtlas;
layout(set = 0, binding = 3) uniform sampler2D depthBuffer; // Scene depth

layout(push_constant) uniform RenderPC {
    mat4  viewProj;
    vec4  camRight;
    vec4  camUp;
    vec2  screenSize;
    float nearPlane;
    float farPlane;
} pc;

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;       // atlas-adjusted UV from vertex shader
layout(location = 2) flat in float fragSoftFade; // soft particle depth-fade distance
layout(location = 3) in vec2 fragQuadUV;   // raw [0,1] quad UV for circle mask

layout(location = 0) out vec4 outColor;

// Reversed-Z: depth buffer stores 1.0 at near and 0.0 at far.
float linearizeDepth(float d) {
    return pc.nearPlane * pc.farPlane / (pc.nearPlane * (1.0 - d) + pc.farPlane * d);
}

void main() {
    vec4 tex = texture(particleAtlas, fragUV);
    
    // Procedural circle mask (softens quad edges using raw quad UV)
    float dist = length(fragQuadUV - 0.5) * 2.0;
    float alphaMask = smoothstep(1.0, 0.8, dist);
    
    vec4 color = tex * fragColor * vec4(1.0, 1.0, 1.0, alphaMask);

    // Soft Particle Depth Fade (configurable distance per emitter)
    float fadeDist = max(fragSoftFade, 0.001);
    vec2 screenUV = gl_FragCoord.xy / pc.screenSize;
    float sceneDepthRaw = texture(depthBuffer, screenUV).r;
    
    float sceneZ = linearizeDepth(sceneDepthRaw);
    float partZ  = linearizeDepth(gl_FragCoord.z);
    
    float diff = sceneZ - partZ;
    float softFade = smoothstep(0.0, fadeDist, diff);
    
    color.a *= softFade;

    if (color.a < 0.01) discard;
    color.rgb = min(color.rgb, vec3(4.0));
    outColor = color;

    // Mild saturation boost for VFX readability
    float lum = dot(outColor.rgb, vec3(0.299, 0.587, 0.114));
    outColor.rgb = mix(vec3(lum), outColor.rgb, 1.1);
}
