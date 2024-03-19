#version 450 core

layout(location = 0) in vec3 inPos;
layout(location = 0) out vec4 outFragColor;

layout(push_constant, row_major) uniform PCR {
	layout(offset = 76) bool shade;
} pcr;

// 9-band spherical harmonics
float sh0(vec3 nrm) { return 0.282095; }
float sh1(vec3 nrm) { return 0.488603 * nrm.y; }
float sh2(vec3 nrm) { return 0.488603 * nrm.z; }
float sh3(vec3 nrm) { return 0.488603 * nrm.x; }
float sh4(vec3 nrm) { return 1.092548 * nrm.x * nrm.y; }
float sh5(vec3 nrm) { return 1.092548 * nrm.y * nrm.z; }
float sh6(vec3 nrm) { return 0.315392 * (3.0 * nrm.z * nrm.z - 1.0); }
float sh7(vec3 nrm) { return 1.092548 * nrm.x * nrm.z; }
float sh8(vec3 nrm) { return 0.546274 * (nrm.x * nrm.x - nrm.y * nrm.y); }

vec3 evalSH(vec3 nrm, vec3 coeffs[9]) {
	return coeffs[0] * sh0(nrm) +
		coeffs[1] * sh1(nrm) +
		coeffs[2] * sh2(nrm) +
		coeffs[3] * sh3(nrm) +
		coeffs[4] * sh4(nrm) +
		coeffs[5] * sh5(nrm) +
		coeffs[6] * sh6(nrm) +
		coeffs[7] * sh7(nrm) +
		coeffs[8] * sh8(nrm);
}

// tried to maximize lighting differences, looks terrible :)
// vec3 shCoeffs[9] = {
// 	vec3(1.0),
// 	vec3(1.0, 0.3, 0.3),
// 	vec3(0.3, 1.0, 0.3),
// 	vec3(0.3, 0.3, 1.0),
// 	vec3(1.0, 0.3, 1.0),
// 	vec3(0.3, 1.0, 1.0),
// 	vec3(1.0, 1.0, 0.3),
// 	vec3(1.0, 1.0, 1.0),
// 	vec3(0.0, 0.0, 0.0)
// };

// taken from convolved sunset atmosphere in iro
vec3 shCoeffs[9] = {
	vec3(1.0),
	vec3(0.257, 0.315, 0.448),
	vec3(-0.123, -0.073, -0.0322),
	vec3(-0.538, -0.319, -0.141),
	vec3(-0.0652, -0.0425, -0.0243),
	vec3(-0.0149, -0.00975, -0.00559),
	vec3(-0.0703, -0.0313, -0.0103),
	vec3(0.0937, 0.0606, 0.0321),
	vec3(0.265, 0.195, 0.114),
};

void main() {
	if(pcr.shade) {
		vec3 n = -normalize(cross(dFdx(inPos), dFdy(inPos)));
		vec3 col = max(evalSH(n, shCoeffs), vec3(0.0));
		outFragColor = vec4(col, 1.0);
	} else {
		outFragColor = vec4(1.0);
	}
}
