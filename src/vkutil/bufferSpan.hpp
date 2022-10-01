#pragma once

#include <fwd.hpp>
#include <vk/vulkan_core.h>
#include <util/allocation.hpp>

namespace vil::vku {

/// Continous non-owned device buffer span.
struct BufferSpan {
	using Allocation = BasicAllocation<VkDeviceSize>;

	VkBuffer buffer {};
	Allocation allocation {};

	bool valid() const { return buffer && allocation.size; }
	VkDeviceSize offset() const { return allocation.offset; }
	VkDeviceSize size() const { return allocation.size; }

	BufferSpan subspan(VkDeviceSize offset, VkDeviceSize size = VK_WHOLE_SIZE);
	std::pair<BufferSpan, BufferSpan> split(VkDeviceSize offset);
};

inline bool operator==(const BufferSpan& a, const BufferSpan& b) {
	return (!a.valid() && !b.valid()) || (a.valid() && b.valid() &&
		a.buffer == b.buffer && a.allocation == b.allocation);
}

inline BufferSpan BufferSpan::subspan(VkDeviceSize offset, VkDeviceSize size) {
	if(size == VK_WHOLE_SIZE) {
		dlg_assert(offset <= allocation.size);
		size = allocation.size - offset;
	}

	dlg_assert(offset + size <= allocation.size);

	auto ret = *this;
	ret.allocation.offset += offset;
	ret.allocation.size = size;

	return ret;
}

inline std::pair<BufferSpan, BufferSpan> BufferSpan::split(VkDeviceSize offset) {
	dlg_assert(offset <= allocation.size);

	auto a = *this;
	a.allocation.size = offset;

	auto b = *this;
	b.allocation.offset += offset;
	b.allocation.size -= offset;

	return {a, b};
}

} // namespace vil::vku
