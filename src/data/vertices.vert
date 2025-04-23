#version 450 core

layout(location = 0) in vec4 inPos;
layout(location = 0) out vec3 outPos;

// layout(set = 0, binding = 0, row_major) uniform CamUbo {
layout(push_constant, row_major) uniform PCR {
	layout(offset = 0) mat4 viewProjMtx;
	layout(offset = 64) bool useW;
	layout(offset = 68) float scale;
	layout(offset = 72) bool flipY;
	// NOTE: offset = 76 already used by fragment shader.
} pcr;

void main() {
	vec3 pos = pcr.useW ? inPos.xyw : inPos.xyz;
	if(pcr.flipY) {
		pos.y *= -1;
	}
	pos *= pcr.scale;
	outPos = pos;

    gl_Position = pcr.viewProjMtx * vec4(pos, 1.0);

	// needed e.g. for pointPipe but also when displaying point data by
	// application
	// TODO: control via pcr
	gl_PointSize = 8.f;
}

