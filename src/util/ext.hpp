#pragma once

#include <vk/vulkan.h>
#include <util/span.hpp>
#include <vector>

namespace vil {

// downgrade
VkImageCopy downgrade(const VkImageCopy2KHR& x);
VkBufferCopy downgrade(const VkBufferCopy2KHR& x);
VkImageBlit downgrade(const VkImageBlit2KHR& x);
VkImageResolve downgrade(const VkImageResolve2KHR& x);
VkBufferImageCopy downgrade(const VkBufferImageCopy2KHR& x);

VkAttachmentDescription downgrade(const VkAttachmentDescription2& x);
VkSubpassDependency downgrade(const VkSubpassDependency2& x);

template<typename D, typename T>
void downgrade(std::vector<D>& dst, span<T> src) {
	dst.reserve(dst.size() + src.size());
	for(auto& x : src) {
		dst.push_back(downgrade(x));
	}
}

template<typename D, typename T>
std::vector<D> downgrade(span<T> src) {
	std::vector<D> dst;
	downgrade(dst, src);
	return dst;
}

// upgrade
VkBufferCopy2KHR upgrade(const VkBufferCopy& x);
VkImageBlit2KHR upgrade(const VkImageBlit& x);
VkImageCopy2KHR upgrade(const VkImageCopy& x);
VkImageResolve2KHR upgrade(const VkImageResolve& x);
VkBufferImageCopy2KHR upgrade(const VkBufferImageCopy& x);

VkAttachmentDescription2 upgrade(const VkAttachmentDescription& x);
VkSubpassDependency2 upgrade(const VkSubpassDependency& x);

template<typename D, typename T>
void upgrade(std::vector<D>& dst, span<T> src) {
	dst.reserve(dst.size() + src.size());
	for(auto& x : src) {
		dst.push_back(upgrade(x));
	}
}

template<typename D, typename T>
void upgrade(std::vector<D>& dst, T* ptr, std::size_t count) {
	upgrade(dst, span<T>(ptr, count));
}

} // namespace vil

