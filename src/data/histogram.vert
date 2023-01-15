#version 460

#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 outChannels;
layout(location = 1) out float outHeight;

#include "histogram.glsl"

layout(set = 0, binding = 0) buffer HistData {
	HistMetadata meta;
	uvec4 data[];
} hist;

void main() {
	vec4 lhist = vec4(0.0);	
	float maxHist = hist.meta.maxHist;
	for(uint i = 0u; i < 3; ++i) {
		lhist[i] = hist.data[gl_InstanceIndex][i] / maxHist;
	}

	outChannels = lhist;

	float m = max(lhist.x, max(lhist.y, max(lhist.z, lhist.w)));
	vec2 start = vec2(
		gl_InstanceIndex / float(hist.data.length()),
		1.f - m);
	vec2 end = vec2(
		(gl_InstanceIndex + 1) / float(hist.data.length()),
		1.f);

	vec2 pos = vec2(
		float((gl_VertexIndex + 1) & 2) * 0.5f,
		float(gl_VertexIndex & 2) * 0.5f);
	outHeight = (1 - pos.y) * m; // 0 or m
	pos = start + pos * (end - start);

	gl_Position = vec4(-1 + 2 * pos, 0.0, 1.0);
}
