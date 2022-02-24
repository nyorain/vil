#version 460

layout(location = 0) in vec4 inChannels;
layout(location = 1) in float inHeight;
layout(location = 0) out vec4 outColor;

void main() {
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

	outColor = vec4(0, 0, 0, 1.0);
	outColor[best] = 1.0;

}
