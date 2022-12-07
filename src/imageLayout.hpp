#pragma once

#include <fwd.hpp>
#include <vk/vulkan.h>
#include <nytl/span.hpp>
#include <vector>

namespace vil {

struct ImageSubresourceLayout {
	VkImageSubresourceRange range;
	VkImageLayout layout;
};

ImageSubresourceLayout initialLayout(const VkImageCreateInfo& ci);
void checkForErrors(span<const ImageSubresourceLayout> state,
	const VkImageCreateInfo& ci);
void simplify(std::vector<ImageSubresourceLayout>& state);
void apply(std::vector<ImageSubresourceLayout>& state,
		span<const ImageSubresourceLayout> changes);
VkImageLayout layout(span<const ImageSubresourceLayout> state,
		VkImageSubresource subres);

} // namespace vil
