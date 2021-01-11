#pragma once

#include <fwd.hpp>
#include <vk/vulkan.h>

namespace fuen {

struct DrawGuiImage {
	enum Type {
		font,
		// custom, uses draw.dsSelected
		e1d,
		e2d,
		e3d,
	};

	// Must match flags in image.frag
	enum Flags : u32 {
		flagMaskR = (1u << 0u),
		flagMaskG = (1u << 1u),
		flagMaskB = (1u << 2u),
		flagMaskA = (1u << 3u),
		flagGrayscale = (1u << 4u),
	};

	Type type;
	// Only relevant when not font
	float layer {};
	float minValue {0.f};
	float maxValue {1.f};
	u32 flags {};
};

} // namespace fuen
