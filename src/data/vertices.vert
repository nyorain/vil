#version 450 core

layout(location = 0) in vec4 inPos;

// layout(set = 0, binding = 0, row_major) uniform CamUbo {
layout(push_constant, row_major) uniform PCR {
	layout(offset = 0) mat4 viewProjMtx;
} cam;

void main() {
	vec3 pos = inPos.xyw;
	pos *= 100;
    gl_Position = cam.viewProjMtx * vec4(pos, 1.0);
}

