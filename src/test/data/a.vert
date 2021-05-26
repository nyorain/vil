// glslangValidator -V --vn a_vert_spv_data a.vert -o a.vert.spv.h

#version 450

layout(constant_id = 0) const uint arraySize = 0u;

layout(location = 0) out vec4 out1;
layout(location = 1) out uint out2;
layout(location = 2) out struct {
	vec4 out3;
	vec4 out4[(arraySize + 4) / 4];
} outStruct;

void main() {
	gl_Position = vec4(1.0, 1.0, 1.0, 1.0);
}
