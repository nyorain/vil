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
	// Iff inUse is true, the fence has payload associated with it (that might
	// already be finished though, putting the fence in signaled state).
	// Waiting on the fence when inUse is false will probably block forever.
	VkFence fence {};
	bool inUse {};

	// descriptor set for selected image view.
	VkDescriptorSet dsSelected {};

	std::vector<Handle*> usedHandles;

	void init(Device& dev);

	Draw() = default;

	Draw(Draw&& rhs) noexcept { swap(*this, rhs); }
	Draw& operator=(Draw rhs) noexcept {
		swap(*this, rhs);
		return *this;
	}

	friend void swap(Draw& a, Draw& b) noexcept;

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
	VkImage image {}; // owned by swapchain
	VkImageView view {};
	VkFramebuffer fb {};

	void init(Device& dev, VkImage img, VkFormat format, VkExtent2D extent, VkRenderPass rp);
	~RenderBuffer();
};

} // namespace fuen
