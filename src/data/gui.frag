#version 450 core

#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(constant_id = 0) const bool outputLinear = true;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D sTexture;

layout(location = 0) in struct {
    vec4 color;
    vec2 uv;
} In;

void main() {
    fragColor = toLinearOpt(In.color * texture(sTexture, In.uv), outputLinear);
}
