#pragma once

#include "device.hpp"
#include <vulkan/vulkan_core.h>
#include <vector>
#include <memory>

namespace fuen {

struct RenderPassDesc {
	std::vector<VkAttachmentDescription> attachments;
	std::vector<VkSubpassDescription> subpasses;
	std::vector<VkSubpassDependency> dependencies;
};

struct RenderPass : DeviceHandle {
	VkRenderPass handle {};

	// Render passes can be destroyed after they were used to create
	// framebuffers or pipelines, the created handles must just be
	// compatible. To know this information, we keep the description
	// of the render pass alive until all associated handles were destroyed.
	std::shared_ptr<RenderPassDesc> desc;
};

struct Framebuffer : DeviceHandle {
	VkFramebuffer handle {};

	std::vector<ImageView*> attachments;
	std::shared_ptr<RenderPassDesc> rp {};

	u32 width {};
	u32 height {};
	u32 layers {};

	~Framebuffer();
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
