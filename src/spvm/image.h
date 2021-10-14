#ifndef __SPIRV_VM_IMAGE_H__
#define __SPIRV_VM_IMAGE_H__

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

struct spvm_state;
struct spvm_image;
struct spvm_result;

typedef struct spvm_vec4f {
	float data[4];
} spvm_vec4f;

typedef struct spvm_image {
	unsigned width;
	unsigned height;
	unsigned depth;
	unsigned layers;
	unsigned levels;

	void* user_data;
} spvm_image;

typedef struct spvm_sampler {
	void* user_data;
} spvm_sampler;

// Default implementation for image sampling
// See VkSamplerCreateInfo
typedef enum spvm_sampler_filter {
	spvm_sampler_filter_nearest,
	spvm_sampler_filter_linear,
} spvm_sampler_filter;

typedef enum spvm_sampler_address_mode {
	spvm_sampler_address_mode_repeat,
	spvm_sampler_address_mode_mirrored_repeat,
	spvm_sampler_address_mode_clamp_to_edge,
	spvm_sampler_address_mode_clamp_to_border,
} spvm_sampler_address_mode;

typedef enum spvm_sampler_compare_op {
	spvm_sampler_compare_op_never,
	spvm_sampler_compare_op_less,
	spvm_sampler_compare_op_equal,
	spvm_sampler_compare_op_less_or_equal,
	spvm_sampler_compare_op_greater,
	spvm_sampler_compare_op_not_equal,
	spvm_sampler_compare_op_greater_or_equal,
	spvm_sampler_compare_op_always,
} spvm_sampler_compare_op;

typedef struct spvm_sampler_desc {
	spvm_sampler_filter filter_min;
	spvm_sampler_filter filter_mag;
	spvm_sampler_filter mipmap_mode;
	spvm_sampler_address_mode address_mode_u;
	spvm_sampler_address_mode address_mode_v;
	spvm_sampler_address_mode address_mode_w;
	float mip_bias;
	char compare_enable;
	spvm_sampler_compare_op compare_op;
	float min_lod;
	float max_lod;
	// TODO: no support for border color
	// TODO: no support for anisotropy yet
	// TODO: no support for unnormalized coordinates yet
} spvm_sampler_desc;

spvm_vec4f spvm_image_read(struct spvm_state*, spvm_image*,
	int x, int y, int z, int layer, int level);
void spvm_image_write(struct spvm_state*, spvm_image*,
	int x, int y, int z, int layer, int level, const spvm_vec4f* data);
spvm_vec4f spvm_sampled_image_sample(struct spvm_state*, spvm_image*, spvm_sampler*,
	float x, float y, float z, float layer, float level);

// spvm_image implementation
// Functions will interpret user_data as float* with tight layout
// and directly read/write it.
spvm_vec4f spvm_image_read_impl(struct spvm_state*, struct spvm_image*,
	int x, int y, int z, int layer, int level);
void spvm_image_write_impl(struct spvm_state*, struct spvm_image*,
	int x, int y, int z, int layer, int level, const spvm_vec4f* data);

// spvm_sampler_image implementation
// Functions will interpret the user_data of the sampler associated
// with the sampled image as spvm_sampler_desc* and sample according to it.
spvm_vec4f spvm_sampled_image_sample_impl(struct spvm_state*, spvm_image*, spvm_sampler*,
	float x, float y, float z, float layer, float level);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // __SPIRV_VM_IMAGE_H__
