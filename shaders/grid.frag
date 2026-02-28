#version 450

layout(location = 0) in vec3 nearPoint;
layout(location = 1) in vec3 farPoint;
layout(location = 2) in vec2 worldXZ;

layout(location = 0) out vec4 outColor;

float gridLine(vec2 coord, float scale) {
    vec2 grid = abs(fract(coord / scale - 0.5) - 0.5) / fwidth(coord / scale);
    float line = min(grid.x, grid.y);
    return 1.0 - min(line, 1.0);
}

void main() {
    // Fine grid (1 unit) and coarse grid (5 units)
    float fine   = gridLine(worldXZ, 1.0);
    float coarse = gridLine(worldXZ, 5.0);

    // Fade with distance from camera
    float dist = length(worldXZ);
    float fade = 1.0 - smoothstep(15.0, 40.0, dist);

    // Axis highlighting: red for X (z ≈ 0), blue for Z (x ≈ 0)
    float axisWidth = 0.06;
    vec3 color = vec3(0.5);
    if (abs(worldXZ.y) < axisWidth) color = vec3(1.0, 0.3, 0.3); // X-axis (red)
    if (abs(worldXZ.x) < axisWidth) color = vec3(0.3, 0.3, 1.0); // Z-axis (blue)

    float alpha = (fine * 0.3 + coarse * 0.5) * fade;
    if (alpha < 0.01) discard;

    outColor = vec4(color, alpha);
}
