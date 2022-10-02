#include <vkutil/sync.hpp>
#include <device.hpp>

namespace vil::vku {

bool operator==(SyncScope a, SyncScope b) {
	return a.stages == b.stages &&
		a.layout == b.layout &&
		a.access == b.access;
}

bool operator!=(SyncScope a, SyncScope b) {
	return a.stages != b.stages ||
		a.layout != b.layout ||
		a.access != b.access;
}

SyncScope& operator|=(SyncScope& a, SyncScope b) {
	if(a.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
		a.layout = b.layout;
	} else if(b.layout != VK_IMAGE_LAYOUT_UNDEFINED && a.layout != b.layout) {
		// Layouts don't match. This isn't a problem for buffers, they
		// don't need layouts. Simply store an invalid layout, we check
		// for that in the barrier functions above.
		a.layout = invalidImageLayout;
	}

	a.stages |= b.stages;
	a.access |= b.access;
	return a;
}

SyncScope operator|(SyncScope a, SyncScope b) {
	return a |= b;
}


void cmdBarrier(Device& dev, VkCommandBuffer cb, span<const ImageBarrier> barriers) {
	return cmdBarrier(dev, cb, barriers, {});
}

void cmdBarrier(Device& dev, VkCommandBuffer cb, span<const BufferBarrier> barriers) {
	return cmdBarrier(dev, cb, {}, barriers);
}

void cmdBarrier(Device& dev, VkCommandBuffer cb, VkImage image, SyncScope src,
		SyncScope dst, VkImageSubresourceRange subres) {
	VkImageMemoryBarrier barrier {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.image = image;
	barrier.srcAccessMask = src.access;
	barrier.oldLayout = src.layout;
	barrier.dstAccessMask = dst.access;
	barrier.newLayout = dst.layout;
	barrier.subresourceRange = subres;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

	dlg_assertm(barrier.oldLayout != invalidImageLayout,
		"Invalid oldLayout: image layout of sync scopes did not match");
	dlg_assertm(barrier.newLayout != invalidImageLayout,
		"Invalid newLayout: image layout of sync scopes did not match");

	dev.dispatch.CmdPipelineBarrier(cb, src.stages, dst.stages,
		0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
}

void cmdBarrier(Device& dev, VkCommandBuffer cb, BufferSpan buf, SyncScope src, SyncScope dst) {
	VkBufferMemoryBarrier barrier {};
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.buffer = buf.buffer;
	barrier.offset = buf.offset();
	barrier.size = buf.size();
	barrier.srcAccessMask = src.access;
	barrier.dstAccessMask = dst.access;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

	dev.dispatch.CmdPipelineBarrier(cb, src.stages, dst.stages,
		0u, 0u, nullptr, 1u, &barrier, 0u, nullptr);
}

void cmdBarrier(Device& dev, VkCommandBuffer cb,
		span<const ImageBarrier> imgBarriers,
		span<const BufferBarrier> bufBarriers) {

	dlg_assert(!imgBarriers.empty() || !bufBarriers.empty());

	VkPipelineStageFlags srcStages = {};
	VkPipelineStageFlags dstStages = {};

	// PERF: use ThreadMemScope
	std::vector<VkImageMemoryBarrier> vkImgBarriers;
	vkImgBarriers.reserve(imgBarriers.size());
	for(auto& b : imgBarriers) {
		if(!b.image) { // empty barriers allowed
			continue;
		}

		srcStages |= b.src.stages;
		dstStages |= b.dst.stages;

		auto& barrier = vkImgBarriers.emplace_back();
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.image = b.image;
		barrier.srcAccessMask = b.src.access;
		barrier.oldLayout = b.src.layout;
		barrier.dstAccessMask = b.dst.access;
		barrier.newLayout = b.dst.layout;
		barrier.subresourceRange = b.subres;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

		dlg_assertm(barrier.oldLayout != invalidImageLayout,
			"Invalid oldLayout: image layout of sync scopes did not match");
		dlg_assertm(barrier.newLayout != invalidImageLayout,
			"Invalid newLayout: image layout of sync scopes did not match");
	}

	// PERF: use ThreadMemScope
	std::vector<VkBufferMemoryBarrier> vkBufBarriers;
	vkBufBarriers.reserve(bufBarriers.size());
	for(auto& b : bufBarriers) {
		if(!b.buffer.buffer) { // empty barriers allowed
			continue;
		}

		srcStages |= b.src.stages;
		dstStages |= b.dst.stages;

		auto& barrier = vkBufBarriers.emplace_back();
		barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		barrier.buffer = b.buffer.buffer;
		barrier.offset = b.buffer.offset();
		barrier.size = b.buffer.size();
		barrier.srcAccessMask = b.src.access;
		barrier.dstAccessMask = b.dst.access;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	}

	dev.dispatch.CmdPipelineBarrier(cb, srcStages, dstStages, 0u, 0u, nullptr,
		vkBufBarriers.size(), vkBufBarriers.data(),
		vkImgBarriers.size(), vkImgBarriers.data());
}

void cmdFullBarrier(Device& dev, VkCommandBuffer cb) {
	auto srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	auto dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

	VkMemoryBarrier memb {};
	memb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	memb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	dev.dispatch.CmdPipelineBarrier(cb, srcStage, dstStage, 0u, 1u, &memb,
		0u, nullptr, 0u, nullptr);
}

bool writes(VkAccessFlags access) {
	auto writing =
		VK_ACCESS_MEMORY_WRITE_BIT |
		VK_ACCESS_HOST_WRITE_BIT |
		VK_ACCESS_SHADER_WRITE_BIT |
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
		VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT |
		VK_ACCESS_COMMAND_PREPROCESS_WRITE_BIT_NV |
    	VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
		VK_ACCESS_TRANSFER_WRITE_BIT;
	return (access & writing) != 0u;
}

} // namespace vil::vku
