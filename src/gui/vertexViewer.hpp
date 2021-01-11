#pragma once

#include <fwd.hpp>
#include <vk/vulkan.h>
#include <vector>

namespace fuen {

struct OwnBuffer {
	Device* dev {};
	VkBuffer buf {};
	VkDeviceMemory mem {};
	VkDeviceSize size {};

	// Will ensure the buffer has at least the given size.
	// If not, will recreate it with the given size and usage.
	// Always uses host visible memory.
	void ensure(Device&, VkDeviceSize, VkBufferUsageFlags);
	~OwnBuffer();
};

struct VertexViewer {
	OwnBuffer ubo_;
	void* uboMap_ {};

	VkDescriptorSetLayout dsLayout_ {};
	VkDescriptorSet ds_ {};
	VkPipelineLayout pipeLayout_ {};

	struct Pipe {
		VkFormat format;
		u32 stride;
		VkPipeline pipe;
	};

	std::vector<Pipe> pipes_ {};
};

} // namespace fuen

