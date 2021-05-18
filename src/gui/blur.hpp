#pragma once

#include <fwd.hpp>

namespace vil {

struct GuiBlur {
	Device* dev;

	VkPipelineLayout pipeLayout;
	VkDescriptorSetLayout dsLayout;
	VkPipeline blurPipe;

	VkDeviceMemory targetMemory;
	VkImage target;
	VkImageView views[2];

	struct RenderBuffer {
		VkImageView view;
		VkDescriptorSet input;
	};

	std::vector<RenderBuffer> renderBuffers;

	VkDescriptorSet steps[2];
	VkExtent2D size;

	~GuiBlur();
};

void init(GuiBlur&, Device&);
void resize(GuiBlur&, VkExtent2D, VkSwapchainKHR swapchain);
void blur(GuiBlur& blur, u32 imageIdx, VkOffset2D offset, VkExtent2D size);

} // namesapce vil
