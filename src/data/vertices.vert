#version 450 core

layout(location = 0) in vec3 inPos;

layout(set = 0, binding = 0, row_major) uniform CamUbo {
	mat4 viewProjMtx;
} cam;

void main() {
    gl_Position = cam.viewProjMtx * vec4(inPos, 1.0);
}

