#version 450 core

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(push_constant) uniform PCR {
    layout(offset = 0) vec2 scale;
    layout(offset = 8) vec2 translate;
} pcr;

layout(location = 0) out struct {
    vec4 color;
    vec2 uv;
} Out;

void main() {
    Out.color = inColor;
    Out.uv = inUV;
    gl_Position = vec4(inPos * pcr.scale + pcr.translate, 0, 1);
}
