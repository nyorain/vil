#version 450 core

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
} pcr;

#ifdef TEX_FORMAT_UINT
	#define SamplerType(Dim) usampler##Dim
#elif defined(TEX_FORMAT_INT)
	#define SamplerType(Dim) isampler##Dim
#elif defined(TEX_FORMAT_FLOAT)
	#define SamplerType(Dim) sampler##Dim
#else
	#error No valid TEX_FORMAT definition
#endif

#ifdef TEX_TYPE_1D_ARRAY
	layout(set = 0, binding = 0) uniform SamplerType(1DArray) sTexture;

	vec4 sampleTex() {
		return textureLod(sTexture, vec2(In.uv.x, pcr.layer), pcr.level);
	}
#elif defined(TEX_TYPE_2D_ARRAY)
	layout(set = 0, binding = 0) uniform SamplerType(2DArray) sTexture;

	vec4 sampleTex() {
		return textureLod(sTexture, vec3(In.uv.xy, pcr.layer), pcr.level);
	}
// #elif TEX_TYPE_CUBE
// TODO: cubearray?
// layout(set = 0, binding = 0) uniform samplerCube sTexture;
#elif defined(TEX_TYPE_3D)
	layout(set = 0, binding = 0) uniform SamplerType(3D) sTexture;

	vec4 sampleTex() {
		return textureLod(sTexture, vec3(In.uv.xy, pcr.layer), pcr.level);
	}
#else
	#error No valid TEX_TYPE definition
#endif

vec3 remap(vec3 val, float oldLow, float oldHigh, float newLow, float newHigh) {
	vec3 t = (val - oldLow) / (oldHigh - oldLow);
	return mix(vec3(newLow), vec3(newHigh), t);
}

void main() {
	vec4 texCol = sampleTex();
	texCol.rgb = remap(texCol.rgb, pcr.valMin, pcr.valMax, 0, 1);

	texCol.r *= float((pcr.flags & flagMaskR) != 0);
	texCol.g *= float((pcr.flags & flagMaskG) != 0);
	texCol.b *= float((pcr.flags & flagMaskB) != 0);
	texCol.a = ((pcr.flags & flagMaskA) != 0) ? texCol.a : 1.f;

	// important for image viewer
	if(In.uv != clamp(In.uv, 0, 1)) {
		texCol.a = 0.f;
	}

	// TODO: add additional luminance mode? might be what some people
	// expect from grayscale
	if((pcr.flags & flagGrayscale) != 0) {
		texCol.rgb = vec3(dot(texCol.rgb, 1.f.xxx));
	}

    fragColor = In.color * texCol;
}

