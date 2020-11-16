#pragma once

#include "device.hpp"
#include <vulkan/vulkan_core.h>
#include <vector>
#include <memory>

namespace fuen {

struct RenderPass : DeviceHandle {
	VkRenderPass handle {};

	struct {
		std::vector<VkAttachmentDescription> attachments;
		std::vector<VkSubpassDescription> subpasses;
		std::vector<VkSubpassDependency> dependencies;
	} info;
};

struct Framebuffer : DeviceHandle {
	VkFramebuffer handle {};

	std::vector<ImageView*> attachments;

	// NOTE: we don't store the renderpass a framebuffer was created
	// with here since the renderpass might be destroyed (and instead a
	// compatible one used in render pass begin). We should just store
	// the relevant render pass info
	// RenderPass* rp;

	u32 width;
	u32 height;
	u32 layers;
};

VKAPI_ATTR VkResult VKAPI_CALL CreateFramebuffer(
    VkDevice                                    device,
    const VkFramebufferCreateInfo*              pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkFramebuffer*                              pFramebuffer);

VKAPI_ATTR void VKAPI_CALL DestroyFramebuffer(
    VkDevice                                    device,
    VkFramebuffer                               framebuffer,
    const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(
    VkDevice                                    device,
    const VkRenderPassCreateInfo*               pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkRenderPass*                               pRenderPass);

VKAPI_ATTR void VKAPI_CALL DestroyRenderPass(
    VkDevice                                    device,
    VkRenderPass                                renderPass,
    const VkAllocationCallbacks*                pAllocator);

} // namespace fuen
