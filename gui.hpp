#pragma once

#include "device.hpp"
#include <variant>

struct ImGuiContext;

namespace fuen {

struct RenderBuffer {
	Device* dev {};
	VkImage image {};
	VkImageView view {};
	VkFramebuffer fb {};

	void init(Device& dev, VkImage img, VkFormat format, VkExtent2D extent, VkRenderPass rp);
	~RenderBuffer();
};

// All data used for drawing a single frame of the overlay.
struct Draw {
	struct Buffer {
		VkBuffer buf {};
		VkDeviceMemory mem {};
		VkDeviceSize size {};

		void ensure(Device& dev, VkDeviceSize, VkBufferUsageFlags);
	};

	Buffer vertexBuffer;
	Buffer indexBuffer;
	VkCommandBuffer cb;
	VkSemaphore semaphore;
	VkFence fence;

	VkDescriptorSet dsSelected;

	void init(Device& dev);
};

struct Renderer {
	Device* dev;
	VkRenderPass rp;
	VkPipeline pipe;

	bool clear {};
	ImGuiContext* imgui {};

	VkDescriptorSet dsFont;

	struct {
		std::variant<
			std::monostate,
			Image*,
			ImageView*,
			Framebuffer*,
			RenderPass*,
			Buffer*,
			DeviceMemory*,
			CommandBuffer*,
			CommandPool*,
			DescriptorPool*,
			DescriptorSet*,
			DescriptorSetLayout*,
			GraphicsPipeline*,
			ComputePipeline*,
			PipelineLayout*,
			ShaderModule*> handle;

		// image data
		struct {
			VkImage handle {};
			VkImageView view {};
			VkImageAspectFlagBits aspectMask {};
		} image;
	} selected;

	struct {
		bool uploaded {};
		VkDeviceMemory mem {};
		VkImage image {};
		VkImageView view {};

		VkDeviceMemory uploadMem {};
		VkBuffer uploadBuf {};
	} font;

public:
	Renderer() = default;
	Renderer(Renderer&&) = delete;
	Renderer& operator=(Renderer&&) = delete;
	~Renderer();

	void init(Device& dev, VkFormat, bool clear);
	void ensureFontAtlas(VkCommandBuffer cb);

	void drawImageUI(Draw&, Image&);
	void drawBufferUI(Draw&, Buffer&);
	void drawSamplerUI(Draw&, Sampler&);
	void drawDescriptorSetUI(Draw&, DescriptorSet&);
	void drawDescriptorPoolUI(Draw&, DescriptorPool&);
	void drawDescriptorSetLayoutUI(Draw&, DescriptorSetLayout&);
	void drawGraphicsPipelineUI(Draw&, GraphicsPipeline&);
	void drawComputePipelineUI(Draw&, ComputePipeline&);
	void drawPipelineLayoutUI(Draw&, PipelineLayout&);
	void drawCommandPoolUI(Draw&, CommandPool&);
	void drawMemoryUI(Draw&, DeviceMemory&);
	void drawCommandBufferUI(Draw&, DeviceMemory&);

	void drawCommandBufferInspector(Draw&, CommandBuffer& cb);

	void drawResourceSelectorUI(Draw&);
	void drawOverviewUI(Draw&);

	void drawGui(Draw&);
	void uploadDraw(Draw&);
	void recordDraw(Draw&, VkExtent2D extent, VkFramebuffer fb, bool force);

	// unselects the given handle, if it was selected.
	void unselect(const Handle& handle);
};

// Can be associated with a VkSurfaceKHR
struct Platform {
	virtual ~Platform() = default;
	virtual void update() = 0;
};

// When rendering directly onto the hooked window
struct Overlay {
	Swapchain* swapchain;
	Renderer renderer;
	std::vector<RenderBuffer> buffers;
	std::vector<Draw> draws;
	Platform* platform;

	void init(Swapchain& swapchain);
	VkResult drawPresent(Queue& queue, span<const VkSemaphore>, u32 imageIdx);
};

} // namespace fuen
