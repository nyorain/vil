#version 460

layout(location = 0) out vec4 outChannels;
layout(location = 1) out float outHeight;

layout(push_constant) uniform PCR {
	uvec2 
} pcr;

layout(set = 0, binding = 0) buffer HistData {
	uint maxHist;
} histData;

layout(set = 0, binding = 1) buffer Hist {
	uint hist[];
} channels[4];

void main() {
	uvec4 hist;	
	for(uint i = 0u; i < 4; ++i) {
		hist[i] = channels[i].hist[gl_InstanceIndex] / float(histData.maxHist);
	}

	outChannels = hist;

	float m = max(hist.x, max(hist.y, max(hist.z, hist.w)));
	vec2 start = vec2(
		gl_InstanceIndex / float(channels[0].hist.length()),
		1.f - m);
	vec2 end = vec2(
		(gl_InstanceIndex + 1) / float(channels[0].hist.length()),
		1.f);

	vec2 pos = vec2(
		float((gl_VertexIndex + 1) & 2) * 0.5f,
		float(gl_VertexIndex & 2) * 0.5f);
	outHeight = (1 - pos.y) * m; // 0 or m
	pos = start + pos * (start - end);

	gl_Position = -1 + 2 * pos;
}
