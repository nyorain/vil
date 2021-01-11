#pragma once

#include <device.hpp>
#include <intrusive.hpp>
#include <gui/commandHook.hpp>

struct ImDrawData;

namespace fuen {

struct ViewableImageCopy;

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
	VkCommandBuffer cb {}; // not freed here, relies on command pool being freed

	// Semaphore associated with the gfx submission of this rendering.
	// Consumed by the present info.
	VkSemaphore presentSemaphore {};

	// Semaphore associated with the gfx submission that can be used later
	// on to make application submissions to a different queue that write
	// to memory we read here wait. When supported by device, this is a timeline
	// semaphore that will be set to the given value by this submission.
	VkSemaphore futureSemaphore {};
	u64 futureSemaphoreValue {};

	// Fence associated with the gfx submission of this rendering.
	// Used to check if frame has completed and Draw can be used again.
	// Iff inUse is true, the fence has payload associated with it (that might
	// already be finished though, putting the fence in signaled state).
	// Waiting on the fence when inUse is false will probably block forever.
	VkFence fence {};
	bool inUse {};

	// descriptor set for selected image view.
	VkDescriptorSet dsSelected {};

	// A list of handles that are referenced by this draw, i.e. are used in
	// the submission. While the associated submission is pending, we must
	// make sure that non of the handles are destroyed or written.
	std::vector<Handle*> usedHandles;
	IntrusivePtr<ViewableImageCopy> keepAliveImageCopy;

	// All the semaphores of submissions (Submission::ourSemaphore) we
	// waited upon. When the draw finishes, they should be returned
	// to the Devices semaphore pool (they are already reset).
	std::vector<VkSemaphore> waitedUpon;

	void init(Device& dev, VkCommandPool pool);

	Draw();
	~Draw();

	Draw(Draw&& rhs) noexcept { swap(*this, rhs); }
	Draw& operator=(Draw rhs) noexcept {
		swap(*this, rhs);
		return *this;
	}

	friend void swap(Draw& a, Draw& b) noexcept;
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
