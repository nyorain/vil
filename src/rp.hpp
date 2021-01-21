#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <vk/vulkan.h>
#include <vector>
#include <memory>

namespace fuen {

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

// Creates 3 RenderPassDescriptions compatible to the given one where
// the only differences are:
// - the first and second renderpasses stores all attachments in the end
// - the second and third renderpasses loads all attachments in the beginning
// This way, one renderpass can be split into three, e.g. to insert
// non-renderpass commands in between (at two points).
// NOTE: the pNext chains are currently lost (needs fix for each extension we want to support)
//   we could probably just forward pNext chains. Evaluate!
struct RenderPassSplitDesc {
	RenderPassDesc desc0;
	RenderPassDesc desc1;
	RenderPassDesc desc2;
};
RenderPassSplitDesc splitInterruptable(const RenderPassDesc&);

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

} // namespace fuen
