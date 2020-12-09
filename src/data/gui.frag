#version 450 core

layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D sTexture;

layout(location = 0) in struct {
    vec4 color;
    vec2 uv;
} In;

void main() {
    fragColor = In.color * texture(sTexture, In.uv);
}
