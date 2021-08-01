#pragma once

#include <fwd.hpp>
#include <gui/render.hpp>
#include <array>

namespace vil {

struct GuiBlur {
	static constexpr auto targetFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

	Device* dev {};

	VkPipelineLayout pipeLayout {};
	VkDescriptorSetLayout dsLayout {};
	VkPipeline blurPipe {};

	VkDeviceMemory targetMemory {};
	VkImage target {};
	VkImageView view0 {};
	VkImageView view1 {};
	VkSampler sampler {};

	struct RenderBuffer {
		VkImage image {};
		VkImageView view {};
		VkDescriptorSet input {};
	};

	std::vector<RenderBuffer> renderBuffers;

	std::array<VkDescriptorSet, 2> steps {};
	VkExtent2D size {};

	OwnBuffer vertices {};

	~GuiBlur();
};

void init(GuiBlur&, Device&);
void destroy(GuiBlur&);
void resize(GuiBlur&, VkExtent2D, VkSwapchainKHR swapchain, VkFormat swapchainFormat);
void blur(GuiBlur& blur, VkCommandBuffer, u32 imageIdx, VkOffset2D offset, VkExtent2D size);

} // namesapce vil
