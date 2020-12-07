#pragma once

#include <device.hpp>

namespace fuen {

struct BufferGui {
	enum class Type {
		f1, f2, f3, f4,
		d1, d2, d3, d4,
		i1, i2, i3, i4,
		u1, u2, u3, u4,
		mat2, mat3, mat4,
		eBool
	};

	Buffer* buffer_;
	VkDeviceSize offset_ {};
	VkDeviceSize size_ {};
	std::vector<std::byte> lastRead_;
	std::vector<Type> layout_;
};

} // namespace fuen
