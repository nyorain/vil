#include "rp.hpp"
#include "data.hpp"
#include "image.hpp"
#include "util.hpp"

namespace fuen {

// Classes
Framebuffer::~Framebuffer() {
	if(!dev) {
		return;
	}

	for(auto* attachment : attachments) {
		auto it = find(attachment->fbs, this);
		dlg_assert(it != attachment->fbs.end());
		attachment->fbs.erase(it);
	}
}

// API
// Framebuffer
VKAPI_ATTR VkResult VKAPI_CALL CreateFramebuffer(
		VkDevice                                    device,
		const VkFramebufferCreateInfo*              pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkFramebuffer*                              pFramebuffer) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& fb = dev.framebuffers.add(*pFramebuffer);
	fb.width = pCreateInfo->width;
	fb.height = pCreateInfo->height;
	fb.layers = pCreateInfo->layers;
	fb.handle = *pFramebuffer;
	fb.objectType = VK_OBJECT_TYPE_FRAMEBUFFER;
	fb.rp = dev.renderPasses.get(pCreateInfo->renderPass).desc;
	fb.dev = &dev;

	for(auto i = 0u; i < pCreateInfo->attachmentCount; ++i) {
		auto& view = dev.imageViews.get(pCreateInfo->pAttachments[i]);
		fb.attachments.emplace_back(&view);

		std::lock_guard lock(dev.mutex);
		view.fbs.push_back(&fb);
	}

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyFramebuffer(
		VkDevice                                    device,
		VkFramebuffer                               framebuffer,
		const VkAllocationCallbacks*                pAllocator) {
	if(!framebuffer) {
		return;
	}

	auto& dev = getData<Device>(device);
	dev.framebuffers.mustErase(framebuffer);
	dev.dispatch.DestroyFramebuffer(device, framebuffer, pAllocator);
}

// RenderPass
VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(
		VkDevice                                    device,
		const VkRenderPassCreateInfo*               pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkRenderPass*                               pRenderPass) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& rp = dev.renderPasses.add(*pRenderPass);
	rp.dev = &dev;
	rp.handle = *pRenderPass;
	rp.objectType = VK_OBJECT_TYPE_RENDER_PASS;

	rp.desc = std::make_shared<RenderPassDesc>();
	rp.desc->subpasses = {pCreateInfo->pSubpasses, pCreateInfo->pSubpasses + pCreateInfo->subpassCount};
	rp.desc->dependencies = {pCreateInfo->pDependencies, pCreateInfo->pDependencies + pCreateInfo->dependencyCount};
	rp.desc->attachments = {pCreateInfo->pAttachments, pCreateInfo->pAttachments + pCreateInfo->attachmentCount};

	return res;
}

VkAttachmentDescription downgrade(const VkAttachmentDescription2& x) {
	struct {
		VkStructureType sType;
		const void* pNext;
		VkAttachmentDescription dst;
	} dst;
	static_assert(sizeof(dst) == sizeof(x));
	std::memcpy(&dst, &x, sizeof(dst));
	return dst.dst;
}

VkSubpassDescription downgrade(const VkSubpassDescription2& x) {
	struct {
		VkStructureType sType;
		const void* pNext;
		VkSubpassDescription dst;
	} dst;
	static_assert(sizeof(dst) == sizeof(x));
	std::memcpy(&dst, &x, sizeof(dst));
	return dst.dst;
}

VkSubpassDependency downgrade(const VkSubpassDependency2& x) {
	struct {
		VkStructureType sType;
		const void* pNext;
		VkSubpassDependency dst;
	} dst;
	static_assert(sizeof(dst) == sizeof(x));
	std::memcpy(&dst, &x, sizeof(dst));
	return dst.dst;
}

template<typename D, typename T>
void downgrade(std::vector<D>& dst, span<const T> src) {
	dst.clear();
	dst.reserve(src.size());
	for(auto& x : src) {
		dst.push_back(downgrade(x));
	}
}

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2(
		VkDevice                                    device,
		const VkRenderPassCreateInfo2*              pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkRenderPass*                               pRenderPass) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& rp = dev.renderPasses.add(*pRenderPass);
	rp.dev = &dev;
	rp.handle = *pRenderPass;
	rp.objectType = VK_OBJECT_TYPE_RENDER_PASS;

	rp.desc = std::make_shared<RenderPassDesc>();
	downgrade(rp.desc->subpasses, span{pCreateInfo->pSubpasses, pCreateInfo->subpassCount});
	downgrade(rp.desc->attachments, span{pCreateInfo->pAttachments, pCreateInfo->attachmentCount});
	downgrade(rp.desc->dependencies, span{pCreateInfo->pDependencies, pCreateInfo->dependencyCount});

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyRenderPass(
		VkDevice                                    device,
		VkRenderPass                                renderPass,
		const VkAllocationCallbacks*                pAllocator) {
	if(!renderPass) {
		return;
	}

	auto& dev = getData<Device>(device);
	dev.dispatch.DestroyRenderPass(device, renderPass, pAllocator);
	dev.renderPasses.mustErase(renderPass);
}

} // namespace fuen
