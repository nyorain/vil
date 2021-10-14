#include <spvm/image.h>
#include <spvm/value.h>
#include <spvm/state.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <math.h>

spvm_vec4f spvm_image_read(struct spvm_state* state, spvm_image* image,
	int x, int y, int z, int layer, int level)
{
	if (state->read_image)
		return state->read_image(state, image, x, y, z, layer, level);

	spvm_vec4f res = {0};
	return res;
}

void spvm_image_write(struct spvm_state* state, spvm_image* image,
	int x, int y, int z, int layer, int level, const spvm_vec4f* data)
{
	if (state->write_image)
		state->write_image(state, image, x, y, z, layer, level, data);
}

spvm_vec4f spvm_sampled_image_sample(struct spvm_state* state, spvm_image* image,
		spvm_sampler* sampler, float x, float y, float z, float layer, float level)
{
	if (state->sample_texel)
		return state->sample_texel(state, image, sampler, x, y, z, layer, level);

	spvm_vec4f res = {0};
	return res;
}

unsigned spvm_image_texel_id(struct spvm_image* image,
	int x, int y, int z, int layer, int level)
{
	unsigned width = image->width;
	unsigned height = image->height;
	unsigned depth = image->depth;
	unsigned off = 0u;

	for(int l = 0; l < level; ++l)
	{
		width = MAX(width >> 1u, 1u);
		height = MAX(height >> 1u, 1u);
		depth = MAX(depth >> 1u, 1u);
		off += width * height * depth * image->layers;
	}

	unsigned sliceSize = width * height;
	unsigned layerSize = depth * sliceSize;

	off += layer * layerSize;
	off += z * sliceSize;
	off += y * width;
	off += x;

	return off;
}

spvm_vec4f spvm_image_read_impl(struct spvm_state* state, struct spvm_image* image,
	int x, int y, int z, int layer, int level)
{
	if (!image->user_data)
	{
		spvm_vec4f res = {0};
		return res;
	}

	unsigned off = spvm_image_texel_id(image, x, y, z, layer, level);
	const float* src = (float*) image->user_data;

	spvm_vec4f res = {0};
	memcpy(res.data, src + off, sizeof(res.data));
	return res;
}

void spvm_image_write_impl(struct spvm_state* state, struct spvm_image* image,
	int x, int y, int z, int layer, int level, const spvm_vec4f* data)
{
	if (!image->user_data)
		return;

	unsigned off = spvm_image_texel_id(image, x, y, z, layer, level);
	float* dst = (float*) image->user_data;
	memcpy(&dst + off, data->data, sizeof(data->data));
}

// For the sampling implementation, see
// https://www.khronos.org/registry/vulkan/specs/1.2-extensions/html/chap16.html#textures-texel-filtering
// There are a lot of corner cases we are not handling currently.

int spvm_util_mirror(int n)
{
	return (n >= 0) ? n : -(1 + n);
}

// Returns -1 for border.
int spvm_apply_address_mode(spvm_sampler_address_mode mode, int val, int size)
{
	switch(mode)
	{
		case spvm_sampler_address_mode_repeat:
			return ((val % size) + size) % size;
		case spvm_sampler_address_mode_clamp_to_edge:
			return CLAMP(val, 0.f, size - 1);
		case spvm_sampler_address_mode_clamp_to_border:
			return (val < 0 || val >= size) ? -1 : val;
		case spvm_sampler_address_mode_mirrored_repeat: {
			int t = ((val % (2 * size)) + 2 * size) % (2 * size) - size;
			return (size - 1) * spvm_util_mirror(t);
		}
	}

	assert(0);
	return -1;
}


float spvm_frac(float val)
{
	double iptr;
	return modf(val, &iptr);
}

spvm_vec4f spvm_fetch_texel(struct spvm_state* state,
	spvm_image* img, const spvm_sampler_desc* desc, int x, int y, int z, int layer, int level)
{
	layer = CLAMP(layer, 0, img->layers - 1);
	level = CLAMP(level, 0, img->levels - 1);

	unsigned width = MAX(img->width >> level, 1u);
	unsigned height = MAX(img->height >> level, 1u);
	unsigned depth = MAX(img->depth >> level, 1u);

	x = spvm_apply_address_mode(x, desc->address_mode_u, width - 1);
	y = spvm_apply_address_mode(y, desc->address_mode_v, height - 1);
	z = spvm_apply_address_mode(z, desc->address_mode_w, depth - 1);

	// check for border condition
	if (x < 0 || y < 0 || z < 0)
	{
		// TODO: allow to customize border color
		spvm_vec4f res = {0.f};
		return res;
	}

	return spvm_image_read_impl(state, img, x, y, z, layer, level);
}

spvm_vec4f spvm_sampled_image_sample_impl(struct spvm_state* state,
	spvm_image* img, spvm_sampler* sampler,
	float s, float t, float r, float layer, float level)
{
	spvm_sampler_desc* desc = (spvm_sampler_desc*) sampler->user_data;
	assert(desc);

	level = CLAMP(level + desc->mip_bias, desc->min_lod, desc->max_lod);
	spvm_sampler_filter filter = (level <= 0.f) ? desc->filter_mag : desc->filter_min;

	int levels[2];
	float level_weights[2];
	unsigned num_level_samples;

	if(desc->mipmap_mode == spvm_sampler_filter_nearest)
	{
		num_level_samples = 1u;
		levels[0] = roundf(level);
		levels[1] = levels[0];
		level_weights[0] = 1.f;
		level_weights[1] = 0.f;
	}
	else
	{
		num_level_samples = 2u;
		levels[0] = floor(level);
		levels[1] = levels[0] + 1;
		level_weights[0] = 1 - (level - levels[0]);
		level_weights[1] = level - levels[0];
	}

	spvm_vec4f res = {0.f};

	for(unsigned l = 0u; l < num_level_samples; ++l)
	{
		unsigned level = CLAMP(levels[l], 0, img->levels - 1);
		unsigned width = MAX(img->width >> level, 1u);
		unsigned height = MAX(img->height >> level, 1u);
		unsigned depth = MAX(img->depth >> level, 1u);

		float u = width * s;
		float v = height * t;
		float w = depth * r;

		const float shift = 0.5f;

		if (filter == spvm_sampler_filter_nearest)
		{
			int i = roundf(u - shift);
			int j = roundf(v - shift);
			int k  = roundf(w - shift);
			spvm_vec4f sample = spvm_fetch_texel(state, img, desc,
				i, j, k, roundf(layer), level);

			for(unsigned j = 0u; j < 4; ++j)
				res.data[j] += level_weights[l] * sample.data[j];
		}
		else
		{
			int i0 = floor(u - shift);
			int j0 = floor(v - shift);
			int k0 = floor(w - shift);

			int i1 = i0 + 1;
			int j1 = j0 + 1;
			int k1 = k0 + 1;

			float alpha = spvm_frac(u - shift);
			float beta = spvm_frac(v - shift);
			float gamma = spvm_frac(w - shift);

			for(unsigned s = 0u; s < 8; ++s)
			{
				int i = (s & 1) ? i0 : i1;
				int j = (s & 2) ? j0 : j1;
				int k = (s & 4) ? k0 : k1;

				float lin_weight =
					((s & 1) ? (1 - alpha) : alpha) *
					((s & 2) ? (1 - beta) : beta) *
					((s & 4) ? (1 - gamma) : gamma);

				spvm_vec4f sample = spvm_fetch_texel(state, img, desc,
					i, j, k, roundf(layer), level);

				for(unsigned j = 0u; j < 4; ++j)
					res.data[j] += lin_weight * level_weights[l] * sample.data[j];
			}

		}
	}

	return res;
}

