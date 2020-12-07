#pragma once

#include "device.hpp"

struct ImDrawData;

namespace fuen {

struct Draw {
	struct Buffer {
		VkBuffer buf {};
		VkDeviceMemory mem {};
		VkDeviceSize size {};

		// Will ensure the buffer has at least the given size.
		// If not, will recreate it with the given size and usage.
		// Always uses host visible memory.
		void ensure(Device&, VkDeviceSize, VkBufferUsageFlags);
		void free(Device&);
	};

	Device* dev {};
	Buffer vertexBuffer {};
	Buffer indexBuffer {};
	// Buffer readbackBuffer; // TODO:
	VkCommandBuffer cb {};

	// Semaphore associated with the gfx submission of this rendering.
	// Consumed by the present info.
	VkSemaphore semaphore {};

	// Fence associated with the gfx submission of this rendering.
	// Used to check if frame has completed and Draw can be used again.
	VkFence fence {};

	VkDescriptorSet dsSelected {};

	void init(Device& dev);
	~Draw();
};

// Static, immutable rendering data shared by all renderers.
struct RenderData {
	VkDescriptorSetLayout dsLayout {};
	VkPipelineLayout pipeLayout {};
	VkSampler linearSampler {};
	VkSampler nearestSampler {};

	void init(Device& dev);
	void free(Device& dev);
};

// For swapchain rendering
struct RenderBuffer {
	Device* dev {};
	VkImage image {};
	VkImageView view {};
	VkFramebuffer fb {};

	void init(Device& dev, VkImage img, VkFormat format, VkExtent2D extent, VkRenderPass rp);
	~RenderBuffer();
};

} // namespace fuen
