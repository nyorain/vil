#version 460

#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(constant_id = 0) const bool outputLinear = true;

layout(location = 0) in struct {
    vec4 color;
    vec2 uv;
} In;

layout(location = 0) out vec4 fragColor;

const uint flagMaskR = (1u << 0u);
const uint flagMaskG = (1u << 1u);
const uint flagMaskB = (1u << 2u);
const uint flagMaskA = (1u << 3u);
const uint flagGrayscale = (1u << 4u);

layout(push_constant) uniform PCR {
	layout(offset = 16) float layer;
	layout(offset = 20) float valMin;
	layout(offset = 24) float valMax;
	layout(offset = 28) uint flags;
	layout(offset = 32) float level;
	layout(offset = 36) float power;
} pcr;

#define SAMPLE_TEX_BINDING 0
#include "sample.glsl"

vec3 remap(vec3 val, float oldLow, float oldHigh, float newLow, float newHigh) {
	vec3 t = (val - oldLow) / (oldHigh - oldLow);
	return mix(vec3(newLow), vec3(newHigh), t);
}

void main() {
	vec4 texCol = sampleTex(vec3(In.uv, pcr.layer), pcr.level);
	texCol.rgb = remap(texCol.rgb, pcr.valMin, pcr.valMax, 0, 1);

	texCol.r *= float((pcr.flags & flagMaskR) != 0);
	texCol.g *= float((pcr.flags & flagMaskG) != 0);
	texCol.b *= float((pcr.flags & flagMaskB) != 0);
	texCol.a = ((pcr.flags & flagMaskA) != 0) ? texCol.a : 1.f;

	// when only alpha is selected, show it as grayscale
	if(pcr.flags == flagMaskA) {
		texCol.rgb = vec3(texCol.a);
		texCol.a = 1.f;
	}

	// important for image viewer
	if(In.uv != clamp(In.uv, 0, 1)) {
		texCol.a = 0.f;
	}

	// TODO: add additional luminance mode? might be what some people
	// expect from grayscale
	if((pcr.flags & flagGrayscale) != 0) {
		texCol.rgb = vec3(dot(texCol.rgb, 0.33f.xxx));
	}

	texCol = pow(texCol, vec4(pcr.power));

    fragColor = toLinearOpt(In.color * texCol, outputLinear);
}

