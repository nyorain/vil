#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <util/intrusive.hpp>
#include <vk/vulkan.h>
#include <vector>
#include <memory>
#include <atomic>

namespace vil {

struct RenderPassDesc {
	std::vector<std::unique_ptr<std::byte[]>> exts; // pNext chains

	std::vector<std::vector<VkAttachmentReference2>> attachmentRefs;
	std::vector<std::vector<u32>> attachmentIDs; // preserve attachment arrays

	std::vector<VkAttachmentDescription2> attachments;
	std::vector<VkSubpassDescription2> subpasses;
	std::vector<VkSubpassDependency2> dependencies;

	// main pNext chain
	const void* pNext {};
	VkRenderPassCreateFlags flags {};
};

struct RenderPass : SharedDeviceHandle {
	static constexpr auto objectType = VK_OBJECT_TYPE_RENDER_PASS;

	VkRenderPass handle {};
	RenderPassDesc desc;
};

struct Framebuffer : SharedDeviceHandle {
	static constexpr auto objectType = VK_OBJECT_TYPE_FRAMEBUFFER;

	VkFramebuffer handle {};

	// NOTE: keep in mind that this might be empty for an imageless fb
	// TODO: makes probably more sense to use IntrusivePtr here and
	//   remove ImageView::fbs (since we don't need to unregister
	//   on destruction anymore).
	std::vector<ImageView*> attachments;
	IntrusivePtr<RenderPass> rp {};

	u32 width {};
	u32 height {};
	u32 layers {};

	bool imageless {};

	~Framebuffer();
};

// Creates 3 RenderPassDescriptions compatible to the given one where
// the only differences are:
// - the first and second renderpasses stores all attachments in the end
// - the second and third renderpasses loads all attachments in the beginning
// This way, one renderpass can be split into three, e.g. to insert
// non-renderpass commands in between (at two points).
// - addSync: whether to add external subpass dependencies making sure that
//   the renderpasses are synchronized as one (i.e. strict ordering of
//   drawing commands).
// NOTE: the pNext chains are currently lost (needs fix for each extension we want to support)
//   we could probably just forward pNext chains. Evaluate!
//   We already warn about it though.
struct RenderPassSplitDesc {
	RenderPassDesc desc0;
	RenderPassDesc desc1;
	RenderPassDesc desc2;
};
RenderPassSplitDesc splitInterruptable(const RenderPassDesc&,
	bool addSync = true, bool addInternalDep = false);

// Returns whether the given renderpass can be split in the given subpass.
// While this is usually possible, there are some combinations of resolve
// attachment placements (when using attachment after resolving) that make
// this impossible.
bool splittable(const RenderPassDesc&, unsigned splitSubpass);

// Creates a new renderpass for the given device with the given description.
VkRenderPass create(Device&, const RenderPassDesc&);

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

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2(
    VkDevice                                    device,
    const VkRenderPassCreateInfo2*              pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkRenderPass*                               pRenderPass);

VKAPI_ATTR void VKAPI_CALL DestroyRenderPass(
    VkDevice                                    device,
    VkRenderPass                                renderPass,
    const VkAllocationCallbacks*                pAllocator);

// No need to hook GetRenderAreaGranularity

} // namespace vil
