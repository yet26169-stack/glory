#version 450

// ── Trail / Ribbon Vertex Shader ───────────────────────────────────────────

struct TrailPoint {
    vec4 posWidth;  // xyz=pos, w=halfWidth
    vec4 colorAge;  // rgb=color, a=age (0=new, 1=old)
};

layout(std430, set=0, binding=0) readonly buffer TrailSSBO {
    TrailPoint points[];
};

layout(push_constant) uniform TrailPC {
    mat4  viewProj;
    vec4  camRight;   // xyz = camera right vector
    vec4  camUp;      // xyz = camera up vector
    uint  pointCount;
    uint  headIndex;
    float widthStart;
    float widthEnd;
} pc;

layout(location=0) out vec4 fragColor;
layout(location=1) out vec2 fragUV;

void main() {
    // Each segment between point[i] and point[i+1] is a quad (6 vertices)
    uint segIndex  = uint(gl_VertexIndex) / 6u;
    uint vertInSeg = uint(gl_VertexIndex) % 6u;

    if (segIndex >= pc.pointCount - 1) {
        gl_Position = vec4(0, 0, 2, 1); // degenerate
        return;
    }

    // In this simplified implementation, we treat the buffer as linear [0..pointCount-1].
    // Real implementation would handle the circular buffer wrap if points > MAX.
    uint iA = segIndex;
    uint iB = segIndex + 1;

    TrailPoint pA = points[iA];
    TrailPoint pB = points[iB];

    // Select which end of the segment this vertex belongs to
    bool isB = (vertInSeg == 1 || vertInSeg == 2 || vertInSeg == 4);
    TrailPoint p = isB ? pB : pA;

    // Side: -1 or +1
    float sides[6] = float[6](-1.0, 1.0, -1.0, -1.0, 1.0, 1.0);
    float side = sides[vertInSeg];

    // Direction between consecutive points for perpendicular expansion
    vec3 diff = pB.posWidth.xyz - pA.posWidth.xyz;
    vec3 segDir = (length(diff) > 0.0001) ? normalize(diff) : vec3(0, 0, 1);
    
    // Use camera-facing expansion (billboard-style ribbon)
    vec3 viewDir = normalize(cross(segDir, pc.camRight.xyz));
    if (length(viewDir) < 0.001) viewDir = pc.camUp.xyz;

    float width = p.posWidth.w;
    vec3 worldPos = p.posWidth.xyz + viewDir * side * width;

    gl_Position = pc.viewProj * vec4(worldPos, 1.0);
    fragColor = p.colorAge;

    // UV: x = 0 or 1 (side), y = age fraction
    float u = (side + 1.0) * 0.5;
    float v = isB ? pB.colorAge.a : pA.colorAge.a;
    fragUV = vec2(u, v);
}
