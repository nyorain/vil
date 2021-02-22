#version 450 core

layout(location = 0) in vec3 inPos;

// layout(set = 0, binding = 0, row_major) uniform CamUbo {
layout(push_constant, row_major) uniform PCR {
	layout(offset = 0) mat4 viewProjMtx;
} cam;

void main() {
    gl_Position = cam.viewProjMtx * vec4(inPos, 1.0);
}

