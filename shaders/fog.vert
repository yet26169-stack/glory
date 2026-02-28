#version 450

// Full-screen triangle from vertex index — no vertex buffer needed
layout(location = 0) out vec2 fragUV;

void main() {
    // Triangle covering entire screen: vertex 0→(-1,-1), 1→(3,-1), 2→(-1,3)
    fragUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(fragUV * 2.0 - 1.0, 0.0, 1.0);
}
