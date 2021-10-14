#pragma once

#include <fwd.hpp>
#include <util/bytes.hpp>
#include <vk/vulkan_core.h>

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
	// - queueFams: the queue families where the buffer will be used.
	//   Passing in duplicates is okay, they will be eliminated.
	//   If queueFams contains less than 2 unique families, exclusive
	//   sharing mode will be used.
	void ensure(Device&, VkDeviceSize, VkBufferUsageFlags,
		span<const u32> queueFams = {});

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
