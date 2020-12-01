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
	VkSemaphore semaphore {};
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

// Manages all rendering of an ImGui
class Renderer {
public:
	Renderer() = default;
	Renderer(Renderer&&) = delete;
	Renderer& operator=(Renderer&&) = delete;
	~Renderer();

	void init(Device& dev, VkFormat, bool clear);
	void uploadDraw(Draw&, const ImDrawData&);
	void recordDraw(Draw&, VkExtent2D extent, VkFramebuffer fb,
		bool drawEvenWhenEmpty, const ImDrawData&);

	Device& dev() const { return *dev_; }
	VkRenderPass rp() const { return rp_; }
	VkPipeline pipe() const { return pipe_; }

private:
	void ensureFontAtlas(VkCommandBuffer cb);

private:
	Device* dev_ {};
	VkRenderPass rp_ {};
	VkPipeline pipe_ {};
	VkCommandPool commandPool_ {};

	bool clear_ {};
	VkDescriptorSet dsFont_ {};

	struct {
		bool uploaded {};
		VkDeviceMemory mem {};
		VkImage image {};
		VkImageView view {};

		VkDeviceMemory uploadMem {};
		VkBuffer uploadBuf {};
	} font_;
};

} // namespace fuen
