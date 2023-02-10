#pragma once

#include <fwd.hpp>
#include <vk/vulkan.h>
#include <nytl/span.hpp>
#include <threadContext.hpp>
#include <vector>

namespace vil {

struct ImageSubresourceLayout {
	VkImageSubresourceRange range;
	VkImageLayout layout;
};

// returns the final number of elements in 'state', after erasure
u32 doSimplify(span<ImageSubresourceLayout> state);

// returns the final number of elements in 'state' as well as the
// new resulting elements
std::pair<span<ImageSubresourceLayout>, u32> doApply(ThreadMemScope& tms,
		span<ImageSubresourceLayout> state,
		const ImageSubresourceLayout& change);

// Resolve references to VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS
void resolve(VkImageSubresourceRange& range, const VkImageCreateInfo& ci);

template<typename A>
void simplify(std::vector<ImageSubresourceLayout, A>& state) {
	auto newSize = doSimplify(state);
	state.resize(newSize);
}

template<typename A>
void apply(std::vector<ImageSubresourceLayout, A>& state,
		span<const ImageSubresourceLayout> changes) {
	for(const auto& change : changes) {
		ThreadMemScope tms;
		auto [newStates, newSize] = doApply(tms, state, change);
		state.resize(newSize);
		state.insert(state.end(), newStates.begin(), newStates.end());
	}

	// We don't simplify here to leave it up to the caller
}

ImageSubresourceLayout initialLayout(const VkImageCreateInfo& ci);
void checkForErrors(span<const ImageSubresourceLayout> state,
	const VkImageCreateInfo& ci);
VkImageLayout layout(span<const ImageSubresourceLayout> state,
		VkImageSubresource subres);

} // namespace vil
