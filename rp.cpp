#include "rp.hpp"
#include "data.hpp"

namespace fuen {

// Framebuffer
VKAPI_ATTR VkResult VKAPI_CALL CreateFramebuffer(
		VkDevice                                    device,
		const VkFramebufferCreateInfo*              pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkFramebuffer*                              pFramebuffer) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.vkCreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& fb = dev.framebuffers.add(*pFramebuffer);
	// fb.rp = dev.renderPasses.find(pCreateInfo->renderPass);
	// dlg_assert(fb.rp);
	fb.width = pCreateInfo->width;
	fb.height = pCreateInfo->height;
	fb.layers = pCreateInfo->layers;
	fb.fb = *pFramebuffer;
	fb.dev = &dev;

	for(auto i = 0u; i < pCreateInfo->attachmentCount; ++i) {
		auto view = pCreateInfo->pAttachments[i];
		fb.attachments.emplace_back(dev.imageViews.find(view));
	}

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyFramebuffer(
		VkDevice                                    device,
		VkFramebuffer                               framebuffer,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.dispatch.vkDestroyFramebuffer(device, framebuffer, pAllocator);
	dev.framebuffers.mustErase(framebuffer);
}

// RenderPass
VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(
		VkDevice                                    device,
		const VkRenderPassCreateInfo*               pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkRenderPass*                               pRenderPass) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.vkCreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& rp = dev.renderPasses.add(*pRenderPass);
	rp.dev = &dev;
	rp.rp = *pRenderPass;
	rp.info.subpasses = {pCreateInfo->pSubpasses, pCreateInfo->pSubpasses + pCreateInfo->subpassCount};
	rp.info.dependencies = {pCreateInfo->pDependencies, pCreateInfo->pDependencies + pCreateInfo->dependencyCount};
	rp.info.attachments = {pCreateInfo->pAttachments, pCreateInfo->pAttachments + pCreateInfo->attachmentCount};

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyRenderPass(
		VkDevice                                    device,
		VkRenderPass                                renderPass,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.dispatch.vkDestroyRenderPass(device, renderPass, pAllocator);
	dev.renderPasses.mustErase(renderPass);
}

} // namespace fuen
