#include <util/ext.hpp>
#include <util/dlg.hpp>

namespace vil {

// downgrade
VkImageCopy downgrade(const VkImageCopy2KHR& x) {
	dlg_assert(!x.pNext);
	return {
		x.srcSubresource,
		x.srcOffset,
		x.dstSubresource,
		x.dstOffset,
		x.extent
	};
}

VkBufferCopy downgrade(const VkBufferCopy2KHR& x) {
	dlg_assert(!x.pNext);
	return {
		x.srcOffset,
		x.dstOffset,
		x.size,
	};
}

VkImageBlit downgrade(const VkImageBlit2KHR& x) {
	dlg_assert(!x.pNext);
	return {
		x.srcSubresource,
		x.srcOffsets[0], x.srcOffsets[1],
		x.dstSubresource,
		x.dstOffsets[0], x.dstOffsets[1],
	};
}

VkImageResolve downgrade(const VkImageResolve2KHR& x) {
	dlg_assert(!x.pNext);
	return {
		x.srcSubresource,
		x.srcOffset,
		x.dstSubresource,
		x.dstOffset,
		x.extent,
	};
}

VkBufferImageCopy downgrade(const VkBufferImageCopy2KHR& x) {
	dlg_assert(!x.pNext);
	return {
		x.bufferOffset,
		x.bufferRowLength,
		x.bufferImageHeight,
		x.imageSubresource,
		x.imageOffset,
		x.imageExtent,
	};
}

VkAttachmentDescription downgrade(const VkAttachmentDescription2& x) {
	dlg_assert(!x.pNext);
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
	dlg_assert(!x.pNext);
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

// upgrade
VkImageCopy2KHR upgrade(const VkImageCopy& x) {
	return {
		VK_STRUCTURE_TYPE_IMAGE_COPY_2_KHR,
		nullptr,
		x.srcSubresource,
		x.srcOffset,
		x.dstSubresource,
		x.dstOffset,
		x.extent
	};
}

VkBufferCopy2KHR upgrade(const VkBufferCopy& x) {
	return {
		VK_STRUCTURE_TYPE_BUFFER_COPY_2_KHR,
		nullptr,
		x.srcOffset,
		x.dstOffset,
		x.size,
	};
}

VkImageBlit2KHR upgrade(const VkImageBlit& x) {
	return {
		VK_STRUCTURE_TYPE_IMAGE_BLIT_2_KHR,
		nullptr,
		x.srcSubresource,
		x.srcOffsets[0], x.srcOffsets[1],
		x.dstSubresource,
		x.dstOffsets[0], x.dstOffsets[1],
	};
}

VkImageResolve2KHR upgrade(const VkImageResolve& x) {
	return {
		VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2_KHR,
		nullptr,
		x.srcSubresource,
		x.srcOffset,
		x.dstSubresource,
		x.dstOffset,
		x.extent,
	};
}

VkBufferImageCopy2KHR upgrade(const VkBufferImageCopy& x) {
	return {
		VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2_KHR,
		nullptr,
		x.bufferOffset,
		x.bufferRowLength,
		x.bufferImageHeight,
		x.imageSubresource,
		x.imageOffset,
		x.imageExtent,
	};
}

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

} // namespace vil
