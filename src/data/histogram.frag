#version 460

#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(constant_id = 0) const bool outputLinear = true;

layout(location = 0) in vec4 inChannels;
layout(location = 1) in float inHeight;
layout(location = 0) out vec4 outColor;

void main() {
	// additive blending
	outColor = vec4(0, 0, 0, 1.0);
	float sum = 0.0;
	for(uint i = 0u; i < 3; ++i) {
		if(inChannels[i] > inHeight) {
			outColor[i] = 1.0;
			sum += 1.0;
		}
	}

	for(uint i = 0u; i < 3; ++i) {
		outColor[i] /= (1 + 0.1 * sum);
	}

	// overlapping, not-additive
#if 0
	uint best = 10;

	// TODO: handle alpha
	for(uint i = 0u; i < 3; ++i) {
		if(inChannels[i] > inHeight && (best > 3 || inChannels[i] <= inChannels[best])) {
			best = i;
		}
	}

	if(best > 3) {
		discard;
	}


	// NOTE: to conversion to linear needed atm

	outColor = vec4(0, 0, 0, 1.0);
	outColor[best] = 1.0;
#endif

	outColor.rgb = toLinearOpt(outColor.rgb, outputLinear);
}
