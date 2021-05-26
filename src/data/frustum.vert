#version 450 core

layout(push_constant, row_major) uniform PCR {
	layout(offset = 0) mat4 viewProjMtx;
	layout(offset = 64) float near;
	layout(offset = 68) float far;
	layout(offset = 72) uint useW;
} pcr;

void main() {
	// Buffer-free cube line list generation. Needs 24 vertices in draw call
	int mask = (1 << gl_VertexIndex);

	// x: 00 10 11 01 00 10 11 01 00 11 11 00
	// y: 10 11 01 00 10 11 01 00 11 11 00 00
	// z: 11 11 11 11 00 00 00 00 01 01 01 01
	float x = 2 * float((0x2D2D3C & mask) != 0) - 1;
	float y = 2 * float((0xB4B4F0 & mask) != 0) - 1;
	float z = pcr.near + (pcr.far - pcr.near) * float((0xFF0055 & mask) != 0);

	if(pcr.useW != 0) {
		x *= z;
		y *= z;
	}

    gl_Position = pcr.viewProjMtx * vec4(x, y, z, 1.0);
}
