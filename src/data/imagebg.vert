#version 450

// origin is top left
layout(location = 0) out vec2 uv;

// the vertex values to use
// they form a square to be drawn
const vec2[] values = {
	{-1, -1}, // 4 outlining points ...
	{1, -1},
	{1, 1},
	{-1, 1},
};

void main() {
	vec2 pos = values[gl_VertexIndex % 4];
	uv = 0.5 + 0.5 * pos;
	gl_Position = vec4(pos, 0.0, 1.0);
}

