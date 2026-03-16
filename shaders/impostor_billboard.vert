#version 450

// Impostor billboard vertex shader.
// Generates a screen-aligned quad per instance from 6 hard-coded vertices.
// Instance data provides world position, scale, UV rect, and tint.

// ── Push constant ──────────────────────────────────────────────────────────
layout(push_constant) uniform PC {
    mat4 viewProj;
};

// ── Per-instance attributes (VK_VERTEX_INPUT_RATE_INSTANCE) ────────────────
layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in float inScale;
layout(location = 2) in vec4 inUVRect;   // xy = min UV, zw = max UV
layout(location = 3) in vec4 inTint;

// ── Outputs to fragment shader ─────────────────────────────────────────────
layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragTint;

void main() {
    // 6 vertices → 2 triangles forming a quad
    //   0--1    Tri 0: 0-1-2
    //   |\ |    Tri 1: 2-1-3
    //   2--3
    const vec2 offsets[6] = vec2[6](
        vec2(-0.5, 1.0),  // 0: top-left
        vec2( 0.5, 1.0),  // 1: top-right
        vec2(-0.5, 0.0),  // 2: bottom-left
        vec2(-0.5, 0.0),  // 2: bottom-left
        vec2( 0.5, 1.0),  // 1: top-right
        vec2( 0.5, 0.0)   // 3: bottom-right
    );

    const vec2 uvs[6] = vec2[6](
        vec2(0.0, 0.0),  // top-left
        vec2(1.0, 0.0),  // top-right
        vec2(0.0, 1.0),  // bottom-left
        vec2(0.0, 1.0),  // bottom-left
        vec2(1.0, 0.0),  // top-right
        vec2(1.0, 1.0)   // bottom-right
    );

    vec2 off = offsets[gl_VertexIndex];
    vec2 uv  = uvs[gl_VertexIndex];

    // Billboard: extract right and up vectors from viewProj inverse.
    // Cheaper: use the top-left 3x3 of viewProj to get camera axes.
    // The viewProj columns (transposed rows) give us:
    //   col0.xyz ≈ right, col1.xyz ≈ up (not exact for perspective but
    //   close enough for isometric camera).
    vec3 right = normalize(vec3(viewProj[0][0], viewProj[1][0], viewProj[2][0]));
    vec3 up    = normalize(vec3(viewProj[0][1], viewProj[1][1], viewProj[2][1]));

    vec3 worldPos = inWorldPos
                  + right * off.x * inScale
                  + up    * off.y * inScale;

    gl_Position = viewProj * vec4(worldPos, 1.0);

    // Map local UV [0,1] to atlas sub-rect
    fragUV   = mix(inUVRect.xy, inUVRect.zw, uv);
    fragTint = inTint;
}
