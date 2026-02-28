#version 450

layout(location = 0) in  vec4 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    // Radial fade from center of point sprite
    vec2 coord = gl_PointCoord * 2.0 - 1.0;
    float r = dot(coord, coord);
    if (r > 1.0) discard;

    float alpha = fragColor.a * (1.0 - r);
    outColor = vec4(fragColor.rgb * alpha, alpha);
}
