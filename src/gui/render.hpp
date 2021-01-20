#pragma once

#include <fwd.hpp>
#include <util/intrusive.hpp>
#include <vk/vulkan.h>
#include <vector>

struct ImDrawData;

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

	OwnBuffer() = default;
	~OwnBuffer();

	OwnBuffer(OwnBuffer&& rhs) noexcept { swap(*this, rhs); }
	OwnBuffer& operator=(OwnBuffer rhs) noexcept {
		swap(*this, rhs);
		return *this;
	}

	friend void swap(OwnBuffer& a, OwnBuffer& b) noexcept;
};

struct Draw {
	Device* dev {};
	OwnBuffer vertexBuffer {};
	OwnBuffer indexBuffer {};
	VkCommandBuffer cb {}; // not freed here, relies on command pool being freed

	struct {
		OwnBuffer copy;
		VkBuffer src {};
		VkDeviceSize offset {};
		VkDeviceSize size {};
		void* map {};
	} readback;

	// Semaphore associated with the gfx submission of this rendering.
	// Consumed by the present info.
	VkSemaphore presentSemaphore {};

	// Semaphore associated with the gfx submission that can be used later
	// on to make application submissions to a different queue that write
	// to memory we read here wait. When supported by device, this is a timeline
	// semaphore that will be set to the given value by this submission.
	VkSemaphore futureSemaphore {};
	u64 futureSemaphoreValue {}; // only for timeline semaphores
	bool futureSemaphoreUsed {}; // only for binary semaphores

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
	std::vector<DeviceHandle*> usedHandles;
	IntrusivePtr<CommandHookState> usedHookState;

	// All the semaphores of submissions (Submission::ourSemaphore) we
	// waited upon. When the draw finishes, they should be returned
	// to the Devices semaphore pool (they are already reset).
	std::vector<VkSemaphore> waitedUpon;

	void init(Device& dev, VkCommandPool pool);

	Draw();
	~Draw();

	Draw(Draw&& rhs) noexcept;
	Draw& operator=(Draw rhs) noexcept;

	friend void swap(Draw& a, Draw& b) noexcept;
};

// Static, immutable rendering data shared by all renderers.
// TODO: this should probably just be moved inside Gui, we don't
// have multiple gui objects.
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