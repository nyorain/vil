#pragma once

#include <vk/vulkan.h>
#include <util/dlg.hpp>
#include <util/linalloc.hpp>
#include <nytl/span.hpp>
#include <vector>

namespace vil {

// downgrade
VkImageCopy downgrade(const VkImageCopy2KHR& x);
VkBufferCopy downgrade(const VkBufferCopy2KHR& x);
VkImageBlit downgrade(const VkImageBlit2KHR& x);
VkImageResolve downgrade(const VkImageResolve2KHR& x);
VkBufferImageCopy downgrade(const VkBufferImageCopy2KHR& x);

VkImageMemoryBarrier downgrade(const VkImageMemoryBarrier2& x);
VkBufferMemoryBarrier downgrade(const VkBufferMemoryBarrier2& x);
VkMemoryBarrier downgrade(const VkMemoryBarrier2& x);

VkAttachmentDescription downgrade(const VkAttachmentDescription2& x);
VkSubpassDependency downgrade(const VkSubpassDependency2& x);

inline VkPipelineStageFlags downgradePipelineStageFlags(VkPipelineStageFlags2 flags) {
	// make sure we don't truncate any new pipeline stages here
	dlg_assert(flags < (u64(1u) << 32));
	return VkPipelineStageFlags(flags);
}

inline VkPipelineStageFlagBits downgradePipelineStageBits(VkPipelineStageFlagBits2 flags) {
	dlg_assert(flags < (u64(1u) << 32));
	return VkPipelineStageFlagBits(flags);
}


template<typename D, typename T>
void downgradeInto(std::vector<D>& dst, span<T> src) {
	dst.reserve(dst.size() + src.size());
	for(auto& x : src) {
		dst.push_back(downgrade(x));
	}
}

template<typename D, typename T>
std::vector<D> downgrade(span<T> src) {
	std::vector<D> dst;
	downgradeInto(dst, src);
	return dst;
}

// upgrade
VkBufferCopy2KHR upgrade(const VkBufferCopy& x);
VkImageBlit2KHR upgrade(const VkImageBlit& x);
VkImageCopy2KHR upgrade(const VkImageCopy& x);
VkImageResolve2KHR upgrade(const VkImageResolve& x);
VkBufferImageCopy2KHR upgrade(const VkBufferImageCopy& x);

VkImageMemoryBarrier2 upgrade(const VkImageMemoryBarrier& x,
	VkPipelineStageFlags src, VkPipelineStageFlags dst);
VkBufferMemoryBarrier2 upgrade(const VkBufferMemoryBarrier& x,
	VkPipelineStageFlags src, VkPipelineStageFlags dst);
VkMemoryBarrier2 upgrade(const VkMemoryBarrier& x,
	VkPipelineStageFlags src, VkPipelineStageFlags dst);

VkAttachmentDescription2 upgrade(const VkAttachmentDescription& x);
VkSubpassDependency2 upgrade(const VkSubpassDependency& x);

inline VkPipelineStageFlags2 upgradePipelineStageFlags(VkPipelineStageFlags flags) {
	return VkPipelineStageFlags2(flags);
}

inline VkPipelineStageFlagBits2 upgradePipelineStageBits(VkPipelineStageFlagBits flags) {
	return VkPipelineStageFlagBits2(flags);
}

template<typename Alloc, typename T, typename... Args>
auto upgrade(Alloc& allocator, span<const T> src, Args&&... args) {
	using D = decltype(upgrade(std::declval<const T&>(), args...));
	auto dst = allocator.template alloc<D>(src.size());
	for(auto i = 0u; i < src.size(); ++i) {
		dst[i] = upgrade(src[i], std::forward<Args>(args)...);
	}
	return dst;
}

template<typename D, typename T>
void upgradeInto(std::vector<D>& dst, span<T> src) {
	dst.reserve(dst.size() + src.size());
	for(auto& x : src) {
		dst.push_back(upgrade(x));
	}
}

template<typename D, typename T>
void upgrade(std::vector<D>& dst, T* ptr, std::size_t count) {
	upgradeInto(dst, span<T>(ptr, count));
}

} // namespace vil

