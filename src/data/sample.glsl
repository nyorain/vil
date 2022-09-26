#ifdef TEX_FORMAT_UINT
	#define SamplerType(Dim) usampler##Dim
	#define StorageTexelBufferType uimageBuffer
	#define RawVec4Type uvec4
	#define DefaultTexelFormat rgba32ui
#elif defined(TEX_FORMAT_INT)
	#define SamplerType(Dim) isampler##Dim
	#define StorageTexelBufferType iimageBuffer
	#define RawVec4Type ivec4
	#define DefaultTexelFormat rgba32i
#elif defined(TEX_FORMAT_FLOAT)
	#define SamplerType(Dim) sampler##Dim
	#define StorageTexelBufferType imageBuffer
	#define RawVec4Type vec4
	#define DefaultTexelFormat rgba32f
#else
	#error No valid TEX_FORMAT definition

	// for linting in editor
	#define SamplerType(Dim) sampler##Dim
	#define StorageTexelBufferType imageBuffer
	#define RawVec4Type vec4
	#define DefaultTexelFormat rgba32f
#endif

// for linting in editor
#if !defined(TEX_TYPE_1D_ARRAY) && !defined(TEX_TYPE_2D_ARRAY) && !defined(TEX_TYPE_3D)
	#define TEX_TYPE_2D_ARRAY
#endif

#ifdef TEX_TYPE_1D_ARRAY
	layout(set = 0, binding = SAMPLE_TEX_BINDING) uniform SamplerType(1DArray) sTexture;

	RawVec4Type fetchTexRaw(ivec3 coords, int level) {
		return texelFetch(sTexture, coords.xy, level);
	}

	vec4 fetchTex(ivec3 coords, int level) {
		return texelFetch(sTexture, coords.xy, level);
	}

	// XXX: here z is array layer while for fetchTex it's y.
	// Kinda confusing but it's the easiest this way for shaders.
	vec4 sampleTex(vec3 coords, float level) {
		return textureLod(sTexture, coords.xz, level);
	}

	ivec3 texSize(int level) {
		return ivec3(textureSize(sTexture, level), 1);
	}
#elif defined(TEX_TYPE_2D_ARRAY)
	layout(set = 0, binding = SAMPLE_TEX_BINDING) uniform SamplerType(2DArray) sTexture;

	RawVec4Type fetchTexRaw(ivec3 coords, int level) {
		return texelFetch(sTexture, coords, level);
	}

	vec4 fetchTex(ivec3 coords, int level) {
		return texelFetch(sTexture, coords, level);
	}

	vec4 sampleTex(vec3 coords, float level) {
		return textureLod(sTexture, coords, level);
	}

	ivec3 texSize(int level) {
		return textureSize(sTexture, level);
	}
#elif defined(TEX_TYPE_3D)
	layout(set = 0, binding = SAMPLE_TEX_BINDING) uniform SamplerType(3D) sTexture;

	RawVec4Type fetchTexRaw(ivec3 coords, int level) {
		return texelFetch(sTexture, coords, level);
	}

	vec4 fetchTex(ivec3 coords, int level) {
		return texelFetch(sTexture, coords, level);
	}

	vec4 sampleTex(vec3 coords, float level) {
		return textureLod(sTexture, coords, level);
	}

	ivec3 texSize(int level) {
		return textureSize(sTexture, level);
	}
#else
	#error No valid TEX_TYPE definition
#endif


bool coordsInside(ivec3 coords, int level) {
	ivec3 s = texSize(level);
	return coords.x < s.x && coords.y < s.y && coords.z < s.z;
}
