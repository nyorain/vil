#include <rp.hpp>
#include <device.hpp>
#include <data.hpp>
#include <image.hpp>
#include <threadContext.hpp>
#include <util/util.hpp>
#include <util/ext.hpp>
#include <vk/format_utils.h>

namespace vil {

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
	auto& dev = getDevice(device);
	auto imageless = pCreateInfo->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;

	LocalVector<VkImageView> viewHandles(pCreateInfo->attachmentCount);
	std::vector<ImageView*> views(pCreateInfo->attachmentCount);

	if(!imageless) {
		for(auto i = 0u; i < pCreateInfo->attachmentCount; ++i) {
			views[i] = &get(dev, pCreateInfo->pAttachments[i]);
			viewHandles[i] = views[i]->handle;
		}
	}

	auto nci = *pCreateInfo;
	nci.pAttachments = viewHandles.data();

	auto res = dev.dispatch.CreateFramebuffer(device, &nci, pAllocator, pFramebuffer);
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
	fb.attachments = std::move(views);
	fb.imageless = imageless;

	for(auto* view : fb.attachments) {
		std::lock_guard lock(dev.mutex);
		view->fbs.push_back(&fb);
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

	auto& dev = getDevice(device);
	dev.framebuffers.mustErase(framebuffer);
	dev.dispatch.DestroyFramebuffer(device, framebuffer, pAllocator);
}

// RenderPass
bool splittable(const RenderPassDesc& desc, unsigned split) {
	dlg_assert(split < desc.subpasses.size());

	// We basically perform symbolic execution of the render pass.
	// Symbolic state per attachment:
	// (attachmentID, subpassID, sequenceID)
	// When an attachment A is written in subpass S, it is moved to
	// state (A, I, 0). Initially all attachments A start in state (A, u32(-1), 0).
	// 'sequenceID' is only used to seperate between partially executed
	// render passes, when the split is performed.
	// Splitting is mainly a problem for certain ordering of resolve attachments.
	// When attachment A is resolved into B, the current state of A becomes the
	// current state of B as well.
	// We basically compare a subpass-by-subpass reference execution of the
	// render pass (with current states symbAttsRef) with our splitted
	// execution of the render pass (with current states symbAtts).
	// If a read attachment is in a different state in our split-execution
	// than in the reference execution, unexpected values would be visible
	// to the application and therefore our approach not viable.
	using AttState = std::tuple<u32, u32, u32>;
	std::vector<AttState> symbAttsRef;
	symbAttsRef.resize(desc.attachments.size());
	for(auto a = 0u; a < desc.attachments.size(); ++a) {
		// NOTE: could respect initial layout.
		// Initial content might be undefined, might be relevant.
		// But on the other hand, renderpasses using undefined content
		// are broken anyways.
		symbAttsRef[a] = {u32(a), u32(-1), 0};
	}

	// TODO: consider preserve attachments?
	// TODO: we never respect VK_ATTACHMENT_UNUSED, needs urgent fix!
	// can we really set all unused non-preserve attachments to undefined?

	// Start real render pass, [0, split).
	// Both executions are the same so far.
	for(auto s = 0u; s < split; ++s) {
		auto& subp = desc.subpasses[s];
		for(auto i = 0u; i < subp.colorAttachmentCount; ++i) {
			auto attID = subp.pColorAttachments[i].attachment;
			if(attID != VK_ATTACHMENT_UNUSED) {
				symbAttsRef[attID] = {attID, s, 0};
			}
		}

		if(subp.pDepthStencilAttachment) {
			auto attID = subp.pDepthStencilAttachment->attachment;
			if(attID != VK_ATTACHMENT_UNUSED) {
				symbAttsRef[attID] = {attID, s, 0};
			}
		}

		if(subp.pResolveAttachments) {
			for(auto i = 0u; i < subp.colorAttachmentCount; ++i) {
				auto srcID = subp.pColorAttachments[i].attachment;
				auto dstID = subp.pResolveAttachments[i].attachment;
				if(srcID != VK_ATTACHMENT_UNUSED && dstID != VK_ATTACHMENT_UNUSED) {
					symbAttsRef[dstID] = symbAttsRef[srcID];
				}
			}
		}
	}

	// perform the split
	auto symbAtts = symbAttsRef;

	// correct symbolic id as the render pass isn't finished yet
	auto& subp = desc.subpasses[split];
	for(auto i = 0u; i < subp.colorAttachmentCount; ++i) {
		auto attID = subp.pColorAttachments[i].attachment;
		if(attID != VK_ATTACHMENT_UNUSED) {
			symbAttsRef[attID] = {attID, split, 0};
			symbAtts[attID] = {attID, split, 2};
		}
	}

	if(subp.pDepthStencilAttachment) {
		auto attID = subp.pDepthStencilAttachment->attachment;
		if(attID != VK_ATTACHMENT_UNUSED) {
			symbAttsRef[attID] = {attID, split, 0};
			symbAtts[attID] = {attID, split, 2};
		}
	}

	if(subp.pResolveAttachments) {
		for(auto i = 0u; i < subp.colorAttachmentCount; ++i) {
			auto srcID = subp.pColorAttachments[i].attachment;
			auto dstID = subp.pResolveAttachments[i].attachment;
			if(srcID != VK_ATTACHMENT_UNUSED && dstID != VK_ATTACHMENT_UNUSED) {
				symbAttsRef[dstID] = symbAttsRef[srcID];
				symbAtts[dstID] = symbAtts[srcID];
			}
		}
	}

	// finish rp0 (split, end)
	for(auto s = split + 1; s < desc.subpasses.size(); ++s) {
		auto& subp = desc.subpasses[s];
		// We don't do anything to color/depthStencil attachments here.
		// Unfortunately, resolve operations are automatically done
		if(subp.pResolveAttachments) {
			for(auto i = 0u; i < subp.colorAttachmentCount; ++i) {
				auto srcID = subp.pColorAttachments[i].attachment;
				auto dstID = subp.pResolveAttachments[i].attachment;
				if(srcID != VK_ATTACHMENT_UNUSED && dstID != VK_ATTACHMENT_UNUSED) {
					symbAtts[dstID] = symbAtts[srcID];
				}
			}
		}
	}

	// start rp1 [0, split)
	for(auto s = 0u; s < split; ++s) {
		auto& subp = desc.subpasses[s];
		// We don't do anything to color/depthStencil attachments here.
		// Unfortunately, resolve operations are automatically done
		if(subp.pResolveAttachments) {
			for(auto i = 0u; i < subp.colorAttachmentCount; ++i) {
				auto srcID = subp.pColorAttachments[i].attachment;
				auto dstID = subp.pResolveAttachments[i].attachment;
				if(srcID != VK_ATTACHMENT_UNUSED && dstID != VK_ATTACHMENT_UNUSED) {
					symbAtts[dstID] = symbAtts[srcID];
				}
			}
		}
	}

	// execute our hooked:dst command
	for(auto i = 0u; i < subp.inputAttachmentCount; ++i) {
		auto attID = subp.pInputAttachments[i].attachment;
		if(attID == VK_ATTACHMENT_UNUSED) {
			continue;
		}

		if(symbAtts[attID] != symbAttsRef[attID]) {
			dlg_trace("splittable({}): subpass {} inputAttachment {} ({})", split, split, i, attID);
			return false;
		}
	}

	for(auto i = 0u; i < subp.colorAttachmentCount; ++i) {
		auto attID = subp.pColorAttachments[i].attachment;
		if(attID == VK_ATTACHMENT_UNUSED) {
			continue;
		}

		if(symbAtts[attID] != AttState{attID, split, 2}) {
			dlg_trace("splittable({}): subpass {}, colorAttachment {} ({})", split, split, i, attID);
			return false;
		}

		symbAtts[attID] = {attID, split, 1};
	}

	if(subp.pDepthStencilAttachment) {
		auto attID = subp.pDepthStencilAttachment->attachment;
		if(attID != VK_ATTACHMENT_UNUSED) {
			if(symbAtts[attID] != AttState{attID, split, 2}) {
				dlg_trace("splittable({}): Unexpected depthStencil state for seq 1", split);
				return false;
			}

			symbAtts[attID] = {attID, split, 1};
		}
	}

	if(subp.pResolveAttachments) {
		for(auto i = 0u; i < subp.colorAttachmentCount; ++i) {
			auto srcID = subp.pColorAttachments[i].attachment;
			auto dstID = subp.pResolveAttachments[i].attachment;
			if(srcID != VK_ATTACHMENT_UNUSED && dstID != VK_ATTACHMENT_UNUSED) {
				symbAtts[dstID] = symbAtts[srcID];
			}
		}
	}

	// end rp1 (split, end)
	for(auto s = split + 1; s < desc.subpasses.size(); ++s) {
		auto& subp = desc.subpasses[s];
		// We don't do anything to color/depthStencil attachments here.
		// Unfortunately, resolve operations are automatically done
		if(subp.pResolveAttachments) {
			for(auto i = 0u; i < subp.colorAttachmentCount; ++i) {
				auto srcID = subp.pColorAttachments[i].attachment;
				auto dstID = subp.pResolveAttachments[i].attachment;
				if(srcID != VK_ATTACHMENT_UNUSED && dstID != VK_ATTACHMENT_UNUSED) {
					symbAtts[dstID] = symbAtts[srcID];
				}
			}
		}
	}

	// start rp2, [0, split)
	for(auto s = 0u; s < split; ++s) {
		auto& subp = desc.subpasses[s];
		// We don't do anything to color/depthStencil attachments here.
		// Unfortunately, resolve operations are automatically done
		if(subp.pResolveAttachments) {
			for(auto i = 0u; i < subp.colorAttachmentCount; ++i) {
				auto srcID = subp.pColorAttachments[i].attachment;
				auto dstID = subp.pResolveAttachments[i].attachment;
				if(srcID != VK_ATTACHMENT_UNUSED && dstID != VK_ATTACHMENT_UNUSED) {
					symbAtts[dstID] = symbAtts[srcID];
				}
			}
		}
	}

	// execute rest of split (everything after hooked)
	for(auto i = 0u; i < subp.inputAttachmentCount; ++i) {
		auto attID = subp.pInputAttachments[i].attachment;
		if(attID != VK_ATTACHMENT_UNUSED && symbAtts[attID] != symbAttsRef[attID]) {
			dlg_trace("splittable({}): subpass {} inputAttachment {} ({})", split, split, i, attID);
			return false;
		}
	}

	for(auto i = 0u; i < subp.colorAttachmentCount; ++i) {
		auto attID = subp.pColorAttachments[i].attachment;
		if(attID == VK_ATTACHMENT_UNUSED) {
			continue;
		}

		if(symbAtts[attID] != AttState{attID, split, 1}) {
			dlg_trace("splittable({}): subpass {} colorAttachment {} ({})", split, split, i, attID);
			return false;
		}

		symbAtts[attID] = {attID, split, 0};
	}

	if(subp.pDepthStencilAttachment) {
		auto attID = subp.pDepthStencilAttachment->attachment;
		if(attID != VK_ATTACHMENT_UNUSED) {
			if(symbAtts[attID] != AttState{attID, split, 1}) {
				dlg_trace("splittable({}): Unexpected depthStencil state for seq 0", split);
				return false;
			}

			symbAtts[attID] = {attID, split, 0};
		}
	}

	if(subp.pResolveAttachments) {
		for(auto i = 0u; i < subp.colorAttachmentCount; ++i) {
			auto srcID = subp.pColorAttachments[i].attachment;
			auto dstID = subp.pResolveAttachments[i].attachment;
			if(srcID != VK_ATTACHMENT_UNUSED && dstID != VK_ATTACHMENT_UNUSED) {
				symbAtts[dstID] = symbAtts[srcID];
			}
		}
	}

	// finish real render pass (split, end)
	for(auto s = split + 1; s < desc.subpasses.size(); ++s) {
		auto& subp = desc.subpasses[s];
		for(auto i = 0u; i < subp.inputAttachmentCount; ++i) {
			auto attID = subp.pInputAttachments[i].attachment;
			if (attID == VK_ATTACHMENT_UNUSED) {
				continue;
			}

			if(symbAtts[attID] != symbAttsRef[attID]) {
				dlg_trace("splittable({}): subpass {} input {} ({})", split, s, i, attID);
				return false;
			}
		}

		for(auto i = 0u; i < subp.colorAttachmentCount; ++i) {
			auto attID = subp.pColorAttachments[i].attachment;
			if (attID == VK_ATTACHMENT_UNUSED) {
				continue;
			}

			if(symbAtts[attID] != symbAttsRef[attID]) {
				dlg_trace("splittable({}): subpass {} color {} ({})", split, s, i, attID);
				return false;
			}

			symbAtts[attID] = {attID, s, 0};
			symbAttsRef[attID] = {attID, s, 0};
		}

		if(subp.pResolveAttachments) {
			for(auto i = 0u; i < subp.colorAttachmentCount; ++i) {
				// Already checked this for color attachments
				// auto attID = subp.pColorAttachments[i].attachment;
				// if(symbAtts[attID] != symbAttsRef[attID]) {
				// 	return false;
				// }

				auto srcID = subp.pColorAttachments[i].attachment;
				auto dstID = subp.pResolveAttachments[i].attachment;
				if(srcID != VK_ATTACHMENT_UNUSED && dstID != VK_ATTACHMENT_UNUSED) {
					symbAtts[dstID] = symbAtts[srcID];
					symbAttsRef[dstID] = symbAtts[srcID]; // same as symbAttsRef[srcID]
				}
			}
		}

		if(subp.pDepthStencilAttachment) {
			auto attID = subp.pDepthStencilAttachment->attachment;
			if(attID != VK_ATTACHMENT_UNUSED) {
				if(symbAtts[attID] != symbAttsRef[attID]) {
					dlg_trace("splittable({}): subpass {} depthStencil ({})", split, s, attID);
					return false;
				}

				symbAtts[attID] = {attID, s, 0};
				symbAttsRef[attID] = {attID, s, 0};
			}
		}
	}

	// Check final state of attachments
	for(auto a = 0u; a < desc.attachments.size(); ++a) {
		auto& att = desc.attachments[a];
		auto hasStencil = FormatHasStencil(att.format);

		// final state is only relevant if attachment is stored.
		if(att.storeOp != VK_ATTACHMENT_STORE_OP_STORE &&
				(!hasStencil || att.stencilStoreOp != VK_ATTACHMENT_STORE_OP_STORE)) {
			continue;
		}

		if(symbAtts[a] != symbAttsRef[a]) {
			dlg_trace("splittable({}): final state attachment {}", split, a);
			return false;
		}
	}

	return true;
}

RenderPassSplitDesc splitInterruptable(const RenderPassDesc& desc) {
	RenderPassDesc desc0 {};
	RenderPassDesc desc1 {};
	RenderPassDesc desc2 {};

	desc0.flags = desc.flags;
	desc1.flags = desc.flags;
	desc2.flags = desc.flags;

	// copy subpasses
	auto copyRefs = [](auto* refs, std::size_t count, auto& attRefs) {
		if(!count) {
			return (VkAttachmentReference2*) nullptr;
		}

		auto& atts = attRefs.emplace_back(refs, refs + count);
		for(auto& att : atts) {
			att.pNext = nullptr;
		}

		return atts.data();
	};

	auto fixSubpasses = [&copyRefs](auto& desc) {
		auto& attRefs = desc.attachmentRefs;
		for(auto& subp : desc.subpasses) {
			subp.pNext = nullptr;
			subp.pColorAttachments = copyRefs(subp.pColorAttachments,
				subp.colorAttachmentCount, attRefs);
			subp.pInputAttachments = copyRefs(subp.pInputAttachments,
				subp.inputAttachmentCount, attRefs);
			subp.pDepthStencilAttachment = copyRefs(subp.pDepthStencilAttachment,
				subp.pDepthStencilAttachment ? 1 : 0, attRefs);
			subp.pResolveAttachments = copyRefs(subp.pResolveAttachments,
				subp.pResolveAttachments ? subp.colorAttachmentCount : 0, attRefs);

			if(subp.preserveAttachmentCount) {
				auto& ids = desc.attachmentIDs.emplace_back(subp.pPreserveAttachments,
					subp.pPreserveAttachments + subp.preserveAttachmentCount);
				subp.pPreserveAttachments = ids.data();
			}
		}
	};

	desc0.subpasses = desc.subpasses;
	desc1.subpasses = desc.subpasses;
	desc2.subpasses = desc.subpasses;

	fixSubpasses(desc0);
	fixSubpasses(desc1);
	fixSubpasses(desc2);

	// Copy dependencies
	desc0.dependencies = desc.dependencies;
	desc1.dependencies = desc.dependencies;
	desc2.dependencies = desc.dependencies;

	for(auto& dep : desc0.dependencies) dep.pNext = nullptr;
	for(auto& dep : desc1.dependencies) dep.pNext = nullptr;
	for(auto& dep : desc2.dependencies) dep.pNext = nullptr;

	// Copy attachments.
	// When an attachment is used in both splitted renderpasses, we transition
	// them to a certain layout in between and make sure to preserve
	// the contents via store/load ops
	auto betweenLayout = VK_IMAGE_LAYOUT_GENERAL;
	desc0.attachments = desc.attachments;
	for(auto& att : desc0.attachments) {
		att.pNext = nullptr;
		att.finalLayout = betweenLayout;
		att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	}

	desc1.attachments = desc.attachments;
	for(auto& att : desc1.attachments) {
		att.pNext = nullptr;
		att.initialLayout = betweenLayout;
		att.finalLayout = betweenLayout;
		att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	}

	desc2.attachments = desc.attachments;
	for(auto& att : desc2.attachments) {
		att.pNext = nullptr;
		att.initialLayout = betweenLayout;
		att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	}

	return {std::move(desc0), std::move(desc1), std::move(desc2)};
}

VkRenderPass create(Device& dev, const RenderPassDesc& desc) {
	auto create2 = dev.dispatch.CreateRenderPass2;
	create2 = {};
	VkRenderPass rp {};

	if(create2) {
		VkRenderPassCreateInfo2 rpi {};
		rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
		rpi.pNext = desc.pNext;
		rpi.flags = desc.flags;
		rpi.subpassCount = u32(desc.subpasses.size());
		rpi.pSubpasses = desc.subpasses.data();
		rpi.attachmentCount = u32(desc.attachments.size());
		rpi.pAttachments = desc.attachments.data();
		rpi.dependencyCount = u32(desc.dependencies.size());
		rpi.pDependencies = desc.dependencies.data();

		VK_CHECK(create2(dev.handle, &rpi, nullptr, &rp));
	} else {
		std::vector<VkAttachmentDescription> attachments;
		downgrade(attachments, span<const VkAttachmentDescription2>(desc.attachments));

		std::vector<VkSubpassDependency> dependencies;
		downgrade(dependencies, span<const VkSubpassDependency2>(desc.dependencies));

		std::vector<VkSubpassDescription> subpasses;
		std::vector<std::vector<VkAttachmentReference>> references;

		auto downgradeAttRefs = [&](const VkAttachmentReference2* refs, std::size_t count) {
			if(count == 0) {
				return u32(0);
			}

			auto off = references.back().size();
			for(auto i = 0u; i < count; ++i) {
				auto& attSrc = refs[i];
				auto& attDst = references.back().emplace_back();
				attDst.attachment = attSrc.attachment;
				attDst.layout = attSrc.layout;
			}

			return u32(off);
		};

		for(auto& src : desc.subpasses) {
			auto& dst = subpasses.emplace_back();
			dst = {};
			dst.flags = src.flags;
			dst.colorAttachmentCount = src.colorAttachmentCount;
			dst.inputAttachmentCount = src.inputAttachmentCount;

			dst.preserveAttachmentCount = src.preserveAttachmentCount;
			dst.pPreserveAttachments = src.pPreserveAttachments;

			auto& atts = references.emplace_back();
			auto colorOff = downgradeAttRefs(src.pColorAttachments, src.colorAttachmentCount);
			auto depthOff = downgradeAttRefs(src.pDepthStencilAttachment, src.pDepthStencilAttachment ? 1 : 0);
			auto inputOff = downgradeAttRefs(src.pInputAttachments, src.inputAttachmentCount);

			if(src.pResolveAttachments) {
				auto resolveOff = downgradeAttRefs(src.pResolveAttachments, src.colorAttachmentCount);
				dst.pResolveAttachments = &atts[resolveOff];
			}

			dst.pColorAttachments = &atts[colorOff];
			dst.pInputAttachments = &atts[inputOff];
			if(src.pDepthStencilAttachment) {
				dst.pDepthStencilAttachment = &atts[depthOff];
			}
		}

		VkRenderPassCreateInfo rpi {};
		rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		rpi.pNext = desc.pNext;
		rpi.flags = desc.flags;
		rpi.subpassCount = u32(subpasses.size());
		rpi.pSubpasses = subpasses.data();
		rpi.attachmentCount = u32(attachments.size());
		rpi.pAttachments = attachments.data();
		rpi.dependencyCount = u32(dependencies.size());
		rpi.pDependencies = dependencies.data();

		VK_CHECK(dev.dispatch.CreateRenderPass(dev.handle, &rpi, nullptr, &rp));
	}

	dlg_assert(rp);
	return rp;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(
		VkDevice                                    device,
		const VkRenderPassCreateInfo*               pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkRenderPass*                               pRenderPass) {
	auto& dev = getDevice(device);
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

			if(attSrc.attachment == VK_ATTACHMENT_UNUSED) {
				continue;
			}

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

		dst.pColorAttachments = src.colorAttachmentCount ? &atts[colorOff] : nullptr;
		dst.pInputAttachments = src.inputAttachmentCount ? &atts[inputOff] : nullptr;
		dst.pDepthStencilAttachment = src.pDepthStencilAttachment ? &atts[depthOff] : nullptr;

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
	auto& dev = getDevice(device);
	auto f = dev.dispatch.CreateRenderPass2;
	auto res = f(device, pCreateInfo, pAllocator, pRenderPass);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& rp = dev.renderPasses.add(*pRenderPass);
	rp.dev = &dev;
	rp.handle = *pRenderPass;
	rp.objectType = VK_OBJECT_TYPE_RENDER_PASS;

	auto& rpi = *pCreateInfo;
	rp.desc = std::make_shared<RenderPassDesc>();
	rp.desc->flags = rpi.flags;
	rp.desc->pNext = rpi.pNext;
	rp.desc->exts.push_back(copyChain(rp.desc->pNext));

	rp.desc->attachments = {rpi.pAttachments, rpi.pAttachments + rpi.attachmentCount};
	rp.desc->subpasses = {rpi.pSubpasses, rpi.pSubpasses + rpi.subpassCount};
	rp.desc->dependencies = {rpi.pDependencies, rpi.pDependencies + rpi.dependencyCount};

	for(auto& att : rp.desc->attachments) {
		copyChain(att.pNext, rp.desc->exts.emplace_back());
	}

	for(auto& dep : rp.desc->dependencies) {
		copyChain(dep.pNext, rp.desc->exts.emplace_back());
	}

	auto addAtts = [&](const VkAttachmentReference2* refs, std::size_t count) {
		auto& atts = rp.desc->attachmentRefs.back();
		auto ret = atts.size();
		atts.insert(atts.end(), refs, refs + count);
		return ret;
	};

	for(auto& subp : rp.desc->subpasses) {
		copyChain(subp.pNext, rp.desc->exts.emplace_back());

		auto& atts = rp.desc->attachmentRefs.emplace_back();
		auto colorOff = addAtts(subp.pColorAttachments, subp.colorAttachmentCount);
		auto depthOff = addAtts(subp.pDepthStencilAttachment, subp.pDepthStencilAttachment ? 1 : 0);
		auto inputOff = addAtts(subp.pInputAttachments, subp.inputAttachmentCount);

		if(subp.pResolveAttachments) {
			auto resolveOff = addAtts(subp.pResolveAttachments, subp.colorAttachmentCount);
			subp.pResolveAttachments = &atts[resolveOff];
		}

		subp.pColorAttachments = &atts[colorOff];
		subp.pInputAttachments = &atts[inputOff];

		if(subp.pDepthStencilAttachment) {
			subp.pDepthStencilAttachment = &atts[depthOff];
		}

		if(subp.preserveAttachmentCount) {
			auto& ids = rp.desc->attachmentIDs.emplace_back(subp.pPreserveAttachments,
				subp.pPreserveAttachments + subp.preserveAttachmentCount);
			subp.pPreserveAttachments = ids.data();
		}
	}

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyRenderPass(
		VkDevice                                    device,
		VkRenderPass                                renderPass,
		const VkAllocationCallbacks*                pAllocator) {
	if(!renderPass) {
		return;
	}

	auto& dev = getDevice(device);
	dev.dispatch.DestroyRenderPass(device, renderPass, pAllocator);
	dev.renderPasses.mustErase(renderPass);
}

} // namespace vil
