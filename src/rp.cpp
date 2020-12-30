#include <rp.hpp>
#include <data.hpp>
#include <image.hpp>
#include <util.hpp>
#include <vk/format_utils.h>

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
VkAttachmentDescription2 upgrade(const VkAttachmentDescription& x) {
	VkAttachmentDescription2 ret {};
	ret.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
	ret.finalLayout = x.finalLayout;
	ret.initialLayout = x.initialLayout;
	ret.flags = x.flags;
	ret.format = x.format;
	ret.loadOp = x.loadOp;
	ret.storeOp = x.storeOp;
	ret.stencilLoadOp = x.stencilLoadOp;
	ret.stencilStoreOp = x.stencilStoreOp;
	ret.samples = x.samples;
	return ret;
}

VkSubpassDependency2 upgrade(const VkSubpassDependency& x) {
	VkSubpassDependency2 ret {};
	ret.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
	ret.dstSubpass = x.dstSubpass;
	ret.srcSubpass = x.srcSubpass;
	ret.srcAccessMask = x.srcAccessMask;
	ret.dstAccessMask = x.dstAccessMask;
	ret.viewOffset = {};
	ret.dependencyFlags = x.dependencyFlags;
	ret.srcStageMask = x.srcStageMask;
	ret.dstStageMask = x.dstStageMask;
	return ret;
}

template<typename D, typename T>
void upgrade(std::vector<D>& dst, span<const T> src) {
	dst.reserve(dst.size() + src.size());
	for(auto& x : src) {
		dst.push_back(upgrade(x));
	}
}

template<typename D, typename T>
void upgrade(std::vector<D>& dst, const T* ptr, std::size_t count) {
	upgrade(dst, span<const T>(ptr, count));
}

VkAttachmentDescription downgrade(const VkAttachmentDescription2& x) {
	VkAttachmentDescription ret {};
	ret.finalLayout = x.finalLayout;
	ret.initialLayout = x.initialLayout;
	ret.flags = x.flags;
	ret.format = x.format;
	ret.loadOp = x.loadOp;
	ret.storeOp = x.storeOp;
	ret.stencilLoadOp = x.stencilLoadOp;
	ret.stencilStoreOp = x.stencilStoreOp;
	ret.samples = x.samples;
	return ret;
}

VkSubpassDependency downgrade(const VkSubpassDependency2& x) {
	VkSubpassDependency ret {};
	ret.dstSubpass = x.dstSubpass;
	ret.srcSubpass = x.srcSubpass;
	ret.srcAccessMask = x.srcAccessMask;
	ret.dstAccessMask = x.dstAccessMask;
	ret.dependencyFlags = x.dependencyFlags;
	ret.srcStageMask = x.srcStageMask;
	ret.dstStageMask = x.dstStageMask;
	return ret;
}

template<typename D, typename T>
void downgrade(std::vector<D>& dst, span<const T> src) {
	dst.reserve(dst.size() + src.size());
	for(auto& x : src) {
		dst.push_back(downgrade(x));
	}
}

// tmp dummy test TODO
// VkRenderPass recreate(const RenderPassDesc& desc) {
// 	Device dev; // todo
// 	bool have2 = true;
// 	VkRenderPass rp;
//
// 	if(have2) {
// 		VkRenderPassCreateInfo2 rpi {};
// 		rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
// 		rpi.pNext = desc.pNext;
// 		rpi.flags = desc.flags;
// 		rpi.subpassCount = desc.subpasses.size();
// 		rpi.pSubpasses = desc.subpasses.data();
// 		rpi.attachmentCount = desc.attachments.size();
// 		rpi.pAttachments = desc.attachments.data();
// 		rpi.dependencyCount = desc.dependencies.size();
// 		rpi.pDependencies = desc.dependencies.data();
//
// 		VK_CHECK(dev.dispatch.CreateRenderPass2(dev.handle, &rpi, nullptr, &rp));
// 	} else {
// 		std::vector<VkAttachmentDescription> attachments;
// 		downgrade(attachments, span<const VkAttachmentDescription2>(desc.attachments));
//
// 		std::vector<VkSubpassDependency> dependencies;
// 		downgrade(dependencies, span<const VkSubpassDependency2>(desc.dependencies));
//
// 		std::vector<VkSubpassDescription> subpasses;
// 		std::vector<std::vector<VkAttachmentReference>> references;
//
// 		auto downgradeAttRefs = [&](const VkAttachmentReference2* refs, std::size_t count) {
// 			if(count == 0) {
// 				return u32(0);
// 			}
//
// 			auto off = references.back().size();
// 			for(auto i = 0u; i < count; ++i) {
// 				auto& attSrc = refs[i];
// 				auto& attDst = references.back().emplace_back();
// 				attDst.attachment = attSrc.attachment;
// 				attDst.layout = attSrc.layout;
// 			}
//
// 			return u32(off);
// 		};
//
// 		for(auto& src : desc.subpasses) {
// 			auto& dst = subpasses.emplace_back();
// 			dst = {};
// 			dst.flags = src.flags;
// 			dst.colorAttachmentCount = src.colorAttachmentCount;
// 			dst.inputAttachmentCount = src.colorAttachmentCount;
//
// 			dst.preserveAttachmentCount = src.preserveAttachmentCount;
// 			dst.pPreserveAttachments = src.pPreserveAttachments;
//
// 			auto& atts = references.emplace_back();
// 			auto colorOff = downgradeAttRefs(src.pColorAttachments, src.colorAttachmentCount);
// 			auto depthOff = downgradeAttRefs(src.pDepthStencilAttachment, src.pDepthStencilAttachment ? 1 : 0);
// 			auto inputOff = downgradeAttRefs(src.pInputAttachments, src.inputAttachmentCount);
//
// 			if(src.pResolveAttachments) {
// 				auto resolveOff = downgradeAttRefs(src.pResolveAttachments, src.colorAttachmentCount);
// 				dst.pResolveAttachments = &atts[resolveOff];
// 			}
//
// 			dst.pColorAttachments = &atts[colorOff];
// 			dst.pInputAttachments = &atts[inputOff];
// 			if(src.pDepthStencilAttachment) {
// 				dst.pDepthStencilAttachment = &atts[depthOff];
// 			}
// 		}
//
// 		VkRenderPassCreateInfo rpi {};
// 		rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
// 		rpi.pNext = desc.pNext;
// 		rpi.flags = desc.flags;
// 		rpi.subpassCount = subpasses.size();
// 		rpi.pSubpasses = subpasses.data();
// 		rpi.attachmentCount = attachments.size();
// 		rpi.pAttachments = attachments.data();
// 		rpi.dependencyCount = dependencies.size();
// 		rpi.pDependencies = dependencies.data();
//
// 		VK_CHECK(dev.dispatch.CreateRenderPass(dev.handle, &rpi, nullptr, &rp));
// 	}
//
// 	return rp;
// }

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

	rp.desc->flags = pCreateInfo->flags;
	rp.desc->pNext = pCreateInfo->pNext;
	rp.desc->exts.push_back(copyChain(rp.desc->pNext));

	// deep copy attachments & dependencies
	upgrade(rp.desc->dependencies, pCreateInfo->pDependencies, pCreateInfo->dependencyCount);
	upgrade(rp.desc->attachments, pCreateInfo->pAttachments, pCreateInfo->attachmentCount);

	// deep copy subpasses
	auto upgradeAttRefs = [&](const VkAttachmentReference* refs, std::size_t count) {
		if(count == 0) {
			return u32(0);
		}

		auto off = rp.desc->attachmentRefs.back().size();
		for(auto i = 0u; i < count; ++i) {
			auto& attSrc = refs[i];
			auto& attDst = rp.desc->attachmentRefs.back().emplace_back();
			attDst.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
			attDst.attachment = attSrc.attachment;
			attDst.layout = attSrc.layout;

			auto format = rp.desc->attachments[attDst.attachment].format;
			if(FormatHasDepth(format)) {
				attDst.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
			}
			if(FormatHasStencil(format)) {
				attDst.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
			}
			if(FormatIsColor(format)) {
				attDst.aspectMask |= VK_IMAGE_ASPECT_COLOR_BIT;
			}
		}

		return u32(off);
	};

	for(auto i = 0u; i < pCreateInfo->subpassCount; ++i) {
		auto& src = pCreateInfo->pSubpasses[i];
		auto& dst = rp.desc->subpasses.emplace_back();
		dst = {};

		dst.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
		dst.colorAttachmentCount = src.colorAttachmentCount;
		dst.flags = src.flags;
		dst.pipelineBindPoint = src.pipelineBindPoint;

		dst.colorAttachmentCount = src.colorAttachmentCount;
		dst.inputAttachmentCount = src.inputAttachmentCount;
		dst.preserveAttachmentCount = src.preserveAttachmentCount;

		auto& atts = rp.desc->attachmentRefs.emplace_back();
		auto colorOff = upgradeAttRefs(src.pColorAttachments, src.colorAttachmentCount);
		auto depthOff = upgradeAttRefs(src.pDepthStencilAttachment, src.pDepthStencilAttachment ? 1 : 0);
		auto inputOff = upgradeAttRefs(src.pInputAttachments, src.inputAttachmentCount);

		if(src.pResolveAttachments) {
			auto resolveOff = upgradeAttRefs(src.pResolveAttachments, src.colorAttachmentCount);
			dst.pResolveAttachments = &atts[resolveOff];
		}

		dst.pColorAttachments = &atts[colorOff];
		dst.pInputAttachments = &atts[inputOff];
		if(src.pDepthStencilAttachment) {
			dst.pDepthStencilAttachment = &atts[depthOff];
		}

		if(src.preserveAttachmentCount) {
			rp.desc->attachmentIDs.emplace_back(src.pPreserveAttachments,
				src.pPreserveAttachments + src.preserveAttachmentCount);
			dst.pPreserveAttachments = rp.desc->attachmentIDs.back().data();
		}
	}

	return res;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2(
		VkDevice                                    device,
		const VkRenderPassCreateInfo2*              pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkRenderPass*                               pRenderPass) {
	auto& dev = getData<Device>(device);
	auto f = selectCmd(dev.dispatch.CreateRenderPass2, dev.dispatch.CreateRenderPass2KHR);
	auto res = f(device, pCreateInfo, pAllocator, pRenderPass);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& rp = dev.renderPasses.add(*pRenderPass);
	rp.dev = &dev;
	rp.handle = *pRenderPass;
	rp.objectType = VK_OBJECT_TYPE_RENDER_PASS;

	rp.desc = std::make_shared<RenderPassDesc>();
	dlg_error("Not implemented");

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
