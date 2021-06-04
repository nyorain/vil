#pragma once

#include <fwd.hpp>

namespace vil {

struct OwnBuffer {
	Device* dev {};
	VkBuffer buf {};
	VkDeviceMemory mem {};
	VkDeviceSize size {};
	std::byte* map {};

	// Will ensure the buffer has at least the given size.
	// If not, will recreate it with the given size and usage.
	// Always uses host visible memory.
	void ensure(Device&, VkDeviceSize, VkBufferUsageFlags);

	void invalidateMap();
	void flushMap();
	ReadBuf data() const { return {map, size}; }

	OwnBuffer() = default;
	~OwnBuffer();

	OwnBuffer(OwnBuffer&& rhs) noexcept { swap(*this, rhs); }
	OwnBuffer& operator=(OwnBuffer rhs) noexcept {
		swap(*this, rhs);
		return *this;
	}

	friend void swap(OwnBuffer& a, OwnBuffer& b) noexcept;
};

} // namesapce vil
