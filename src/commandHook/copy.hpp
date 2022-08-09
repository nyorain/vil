#pragma once

#include <fwd.hpp>
#include <variant>
#include <util/ownbuf.hpp>
#include <vk/vulkan_core.h>

namespace vil {

struct CopiedImageToBuffer;

// Common utility
// NOTE: they expect the device mutex to be locked.
void initAndCopy(Device& dev, VkCommandBuffer cb, OwnBuffer& dst,
	VkBufferUsageFlags addFlags, Buffer& src,
	VkDeviceSize srcOffset, VkDeviceSize size, u32 queueFamsBitset);
void performCopy(Device& dev, VkCommandBuffer cb, const Buffer& src,
	VkDeviceSize srcOffset, OwnBuffer& dst, VkDeviceSize dstOffset,
	VkDeviceSize size);
void initAndCopy(Device& dev, VkCommandBuffer cb, CopiedImage& dst, Image& src,
	VkImageLayout srcLayout, VkImageSubresourceRange srcSubres,
	u32 srcQueueFam);
void initAndSampleCopy(Device& dev, VkCommandBuffer cb,
	CopiedImageToBuffer& dst, Image& src, VkImageLayout srcLayout,
	const VkImageSubresourceRange& srcSubres, u32 queueFamsBitset,
	std::vector<VkImageView>& imgViews, std::vector<VkBufferView>& bufViews,
	std::vector<VkDescriptorSet>& dss);

void performCopy(Device& dev, VkCommandBuffer cb, VkDeviceAddress srcPtr,
		OwnBuffer& dst, VkDeviceSize dstOffset, VkDeviceSize size);
void initAndCopy(Device& dev, VkCommandBuffer cb, OwnBuffer& dst,
		VkDeviceAddress srcPtr, VkDeviceSize size, u32 queueFamsBitset);

} // namespace vil
