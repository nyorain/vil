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
	layout(set = 0, binding = SAMPLE_TEX_BINDING) uniform SamplerType(1DArray) sTexture;

	vec4 fetchTex(ivec3 coords, int level) {
		return texelFetch(sTexture, coords.xy, level);
	}

	// XXX: here z is array layer while for fetchTex it's y.
	// Kinda confusing but it's the easiest this way for shaders.
	vec4 sampleTex(vec3 coords, float level) {
		return textureLod(sTexture, coords.xz, level);
	}

	bool coordsInside(ivec3 coords, int level) {
		ivec2 s = textureSize(sTexture, level);
		return coords.x < s.x && coords.y < s.y && coords.z == 0u;
	}
#elif defined(TEX_TYPE_2D_ARRAY)
	layout(set = 0, binding = SAMPLE_TEX_BINDING) uniform SamplerType(2DArray) sTexture;

	vec4 fetchTex(ivec3 coords, int level) {
		return texelFetch(sTexture, coords, level);
	}

	vec4 sampleTex(vec3 coords, float level) {
		return textureLod(sTexture, coords, level);
	}

	bool coordsInside(ivec3 coords, int level) {
		ivec3 s = textureSize(sTexture, level);
		return coords.x < s.x && coords.y < s.y && coords.z < s.z;
	}
#elif defined(TEX_TYPE_3D)
	layout(set = 0, binding = SAMPLE_TEX_BINDING) uniform SamplerType(3D) sTexture;

	vec4 fetchTex(ivec3 coords, int level) {
		return texelFetch(sTexture, coords, level);
	}

	vec4 sampleTex(vec3 coords, float level) {
		return textureLod(sTexture, coords, level);
	}

	bool coordsInside(ivec3 coords, int level) {
		ivec3 s = textureSize(sTexture, level);
		return coords.x < s.x && coords.y < s.y && coords.z < s.z;
	}
#else
	#error No valid TEX_TYPE definition
#endif


