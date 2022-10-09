#pragma once

#include <fwd.hpp>
#include <nytl/bytes.hpp>
#include <nytl/stringParam.hpp>
#include <vk/vulkan_core.h>
#include <vkutil/bufferSpan.hpp>

namespace vil {

struct OwnBuffer {
	enum class Type {
		hostVisible,
		deviceLocal,
	};

	Device* dev {};
	VkBuffer buf {};
	VkDeviceMemory mem {};
	VkDeviceSize size {};
	std::byte* map {};

	// Will ensure the buffer has at least the given size.
	// If not, will recreate it with the given size and usage.
	// - queueFams: the queue families where the buffer will be used.
	//   Passing in duplicates is okay, they will be eliminated.
	//   If queueFams contains less than 2 unique families, exclusive
	//   sharing mode will be used.
	void ensure(Device&, VkDeviceSize, VkBufferUsageFlags,
		u32 queueFamsBitfield = {}, StringParam name = {},
		Type type = Type::hostVisible);

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

	vku::BufferSpan asSpan(VkDeviceSize offset = 0u, VkDeviceSize size = VK_WHOLE_SIZE) const;
	explicit operator vku::BufferSpan() const { return asSpan(); }

	friend void swap(OwnBuffer& a, OwnBuffer& b) noexcept;
};

} // namesapce vil
