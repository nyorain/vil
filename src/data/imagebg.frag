#version 450

#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(constant_id = 0) const bool outputLinear = true;

layout(push_constant) uniform PCR {
    layout(offset = 0) vec2 qsize;
} pcr;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

void main() {
	vec2 scaled = uv;
	uvec2 rest = uvec2(scaled / pcr.qsize);
	if(rest.x % 2 == rest.y % 2) {
		fragColor.rgb = toLinearOpt(vec3(0.02, 0.02, 0.02), outputLinear);
	} else {
		fragColor.rgb = toLinearOpt(vec3(0.1, 0.1, 0.1), outputLinear);
	}

	fragColor.a = 1;
}

