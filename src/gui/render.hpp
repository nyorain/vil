#pragma once

#include <fwd.hpp>
#include <util/intrusive.hpp>
#include <util/ownbuf.hpp>
#include <vk/vulkan.h>
#include <imgui/imgui.h>
#include <vector>

namespace vil {

struct BufferSpan {
	VkBuffer buffer {};
	VkDeviceSize offset {};
	VkDeviceSize size {};
};

struct Draw {
	Device* dev {};
	OwnBuffer vertexBuffer {};
	OwnBuffer indexBuffer {};
	VkCommandBuffer cb {}; // not freed here, relies on command pool being freed

	// Semaphore associated with the gfx submission of this rendering.
	// Consumed by the present info.
	VkSemaphore presentSemaphore {};

	// Only when we are not using timeline semaphores:
	// Semaphore associated with the gfx submission that can be used later
	// on to make application submissions to a different queue that write
	// to memory we read here wait. When supported by device, this is a timeline
	// semaphore that will be set to the given value by this submission.
	VkSemaphore futureSemaphore {};
	bool futureSemaphoreUsed {}; // only for binary semaphores
	bool futureSemaphoreSignaled {}; // only false when draw is used first

	// Synchronization for timeline semaphores: submissionID on gui gfxQueue
	u64 lastSubmissionID {}; // only for timeline semaphores

	// Fence associated with the gfx submission of this rendering.
	// Used to check if frame has completed and Draw can be used again.
	// Iff inUse is true, the fence has payload associated with it (that might
	// already be finished though, putting the fence in signaled state).
	// Waiting on the fence when inUse is false will probably block forever.
	VkFence fence {};
	bool inUse {};

	// A list of handles that are referenced by this draw, i.e. are used in
	// the submission. While the associated submission is pending, we must
	// make sure that none of the handles are destroyed and synchronize future
	// applicaiton submissions that might write them.
	std::vector<DeviceHandle*> usedHandles;
	IntrusivePtr<CommandHookState> usedHookState;

	// All the semaphores of submissions (Submission::ourSemaphore) we
	// waited upon. When the draw finishes, they should be returned
	// to the Devices semaphore pool (they are already reset).
	std::vector<VkSemaphore> waitedUpon;

	// Called from the gui thread with dev mutex locked when this draw finishes.
	// One-shot, i.e. automatically cleared aftewards.
	using FinishCallback = std::function<void(Draw&)>;
	std::vector<FinishCallback> onFinish;

	// frame number in which this draw was last used
	u64 lastUsed {};

	void init(Gui& gui, VkCommandPool pool);

	Draw();
	~Draw();

	Draw(Draw&& rhs) noexcept;
	Draw& operator=(Draw rhs) noexcept;

	friend void swap(Draw& a, Draw& b) noexcept;
};

// For swapchain rendering
struct RenderBuffer {
	Device* dev {};
	VkImage image {}; // owned by swapchain
	VkImageView view {};
	VkFramebuffer fb {};

	void init(Device& dev, VkImage img, VkFormat format, VkExtent2D extent,
		VkRenderPass rp, VkImageView depthView);
	~RenderBuffer();
};

struct DrawGuiImage {
	// The type of the image.
	// When it's ImageShader::count, its' the font texture and all other
	// fields are irrelevant.
	ShaderImageType::Value type;

	// Must match flags in image.frag
	enum Flags : u32 {
		flagMaskR = (1u << 0u),
		flagMaskG = (1u << 1u),
		flagMaskB = (1u << 2u),
		flagMaskA = (1u << 3u),
		flagGrayscale = (1u << 4u),
	};

	// The descriptor to bind for drawing
	VkDescriptorSet ds {};

	// image.frag information:
	// the channels to sample
	u32 flags {
		DrawGuiImage::flagMaskR |
		DrawGuiImage::flagMaskG |
		DrawGuiImage::flagMaskB};

	// min/max values for scaling in shader
	float minValue {0.f};
	float maxValue {1.f};

	// the layer/level to sample
	float layer {};
	float level {};
};

} // namespace vil
