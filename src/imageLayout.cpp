#include <imageLayout.hpp>
#include <util/util.hpp>
#include <threadContext.hpp>

namespace vil {

ImageSubresourceLayout initialLayout(const VkImageCreateInfo& ci) {
	ImageSubresourceLayout ret {};
	ret.range.aspectMask = aspects(ci.format);
	ret.range.layerCount = ci.arrayLayers;
	ret.range.levelCount = ci.mipLevels;
	ret.layout = ci.initialLayout;
	return ret;
}

// NOTE: one call to simplify may not transform it into the most simple
// representation possible. It should converge after multiple repeated calls
// but this is usually not needed.
// That's the reason we do greedy (but never worsening) merging in the
// layer dimension.
// TODO: also simplify for aspect masks
u32 doSimplify(span<ImageSubresourceLayout> state) {
	auto outIt = state.begin();
	auto inIt = state.begin();
	while(inIt != state.end()) {
		auto& sub = *inIt;
		if(sub.range.baseArrayLayer == 0 && sub.range.baseMipLevel == 0) {
			if(inIt != outIt) {
				*outIt = sub;
			}
			++inIt;
			++outIt;
			continue;
		}

		auto erase = false;
		auto subLevelEnd = sub.range.baseMipLevel + sub.range.levelCount;
		for(auto& other : state) {
			if(other.range.aspectMask != sub.range.aspectMask ||
					other.layout != sub.layout) {
				continue;
			}

			auto otherLayerEnd = other.range.baseArrayLayer + other.range.layerCount;
			auto otherLevelEnd = other.range.baseMipLevel + other.range.levelCount;

			// greedy in this dimension
			if(sub.range.baseArrayLayer > 0 &&
					otherLayerEnd == sub.range.baseArrayLayer &&
					// 'sub' covers at least the mip range of 'other'
					other.range.baseMipLevel >= sub.range.baseMipLevel &&
					otherLevelEnd <= subLevelEnd &&
					// make sure that 'sub' is at one end of the mip range
					// of 'other' to avoid inserting additional states
					(other.range.baseMipLevel == sub.range.baseMipLevel ||
					otherLevelEnd == subLevelEnd)) {

				// can merge!
				other.range.layerCount += sub.range.layerCount;

				if(other.range.baseMipLevel == sub.range.baseMipLevel &&
						other.range.levelCount == sub.range.levelCount) {
					erase = true;
				} else if(other.range.baseMipLevel != sub.range.baseMipLevel) {
					// shrink to just beginning
					sub.range.levelCount = other.range.baseMipLevel - sub.range.baseMipLevel;
				} else {
					dlg_assert(subLevelEnd > otherLevelEnd);

					// shrink to just end
					sub.range.levelCount = subLevelEnd - otherLevelEnd;
					sub.range.baseMipLevel = otherLevelEnd;
				}
				break;
			}

			if(sub.range.baseMipLevel > 0 &&
					otherLevelEnd == sub.range.baseMipLevel &&
					other.range.baseArrayLayer == sub.range.baseArrayLayer &&
					other.range.layerCount == sub.range.layerCount) {
				// can merge!
				other.range.levelCount += sub.range.levelCount;
				erase = true;
				break;
			}

		}

		if(erase) {
			++inIt;
		} else {
			if(inIt != outIt) {
				*outIt = sub;
			}
			++inIt;
			++outIt;
		}
	}

	return outIt - state.begin();
}

std::pair<span<ImageSubresourceLayout>, u32> doApply(
		ThreadMemScope& tms,
		span<ImageSubresourceLayout> state,
		const ImageSubresourceLayout& change) {

	dlg_assert(change.range.levelCount != VK_REMAINING_MIP_LEVELS);
	dlg_assert(change.range.layerCount != VK_REMAINING_ARRAY_LAYERS);

	// TODO: figure out multiplane support here. Somewhat complicated though
	// as applications can mix changes of ASPECT_COLOR and plane-specific
	// aspects. Should probably always explicitly store the subresources
	// by plane-aspects for multiplane formats and just replace
	// ASPECT_COLOR changes with all plane bits, the spec says that's how
	// it works.
	constexpr auto supportedAspects =
		VK_IMAGE_ASPECT_COLOR_BIT |
		VK_IMAGE_ASPECT_DEPTH_BIT |
		VK_IMAGE_ASPECT_STENCIL_BIT;
	dlg_assert((change.range.aspectMask & ~supportedAspects) == 0u);

	// check that the change is actually valid
	dlg_assertl_or(dlg_level_warn,
		change.range.aspectMask != 0 &&
		change.range.layerCount > 0 &&
		change.range.levelCount > 0,
		return {});

	// TODO: fast path for *full* overlap with a state

	// cut out 'change.range' from 'range' in all 'state' elements
	auto changeLayerEnd = change.range.baseArrayLayer + change.range.layerCount;
	auto changeLevelEnd = change.range.baseMipLevel + change.range.levelCount;

	dlg_assertlm(dlg_level_warn, state.size() < 1024u,
		"Large ImageLayout state detected");

	auto newStates = tms.alloc<ImageSubresourceLayout>(state.size() * 4 + 1);
	auto numNewStates = 0u;

	auto outIt = state.begin();
	auto inIt = state.begin();
	while(inIt != state.end()) {
		auto subState = *inIt;
		auto aspectIntersection = subState.range.aspectMask & change.range.aspectMask;
		if(aspectIntersection == 0u) {
			if(outIt != inIt) {
				*outIt = *inIt;
			}

			++outIt;
			++inIt;
			continue;
		}

		auto subLayerEnd = subState.range.baseArrayLayer + subState.range.layerCount;
		auto subLevelEnd = subState.range.baseMipLevel + subState.range.levelCount;

		// compute overlap
		auto layerStartO = std::max(
			subState.range.baseArrayLayer,
			change.range.baseArrayLayer);
		auto layerEndO = std::min(subLayerEnd, changeLayerEnd);
		auto levelStartO = std::max(
			subState.range.baseMipLevel,
			change.range.baseMipLevel);
		auto levelEndO = std::min(subLevelEnd, changeLevelEnd);

		// check if the subranges overlap
		if(layerEndO <= layerStartO || levelEndO <= levelStartO) {
			if(outIt != inIt) {
				*outIt = *inIt;
			}

			++outIt;
			++inIt;
			continue;
		}

		// cut layer
		if(subState.range.baseArrayLayer < change.range.baseArrayLayer) {
			auto& newBeginning = newStates[numNewStates++];
			newBeginning.layout = subState.layout;
			newBeginning.range.aspectMask = aspectIntersection;
			newBeginning.range.levelCount = subState.range.levelCount;
			newBeginning.range.baseMipLevel = subState.range.baseMipLevel;
			newBeginning.range.baseArrayLayer = subState.range.baseArrayLayer;
			newBeginning.range.layerCount = layerStartO - subState.range.baseArrayLayer;
		}
		if(subLayerEnd > changeLayerEnd) {
			auto& newEnd = newStates[numNewStates++];
			newEnd.layout = subState.layout;
			newEnd.range.aspectMask = aspectIntersection;
			newEnd.range.levelCount = subState.range.levelCount;
			newEnd.range.baseMipLevel = subState.range.baseMipLevel;
			newEnd.range.baseArrayLayer = layerEndO;
			newEnd.range.layerCount = subLayerEnd - layerEndO;
		}

		// cut level (only needed in overlap)
		if(subState.range.baseMipLevel < change.range.baseMipLevel) {
			auto& newBeginning = newStates[numNewStates++];
			newBeginning.layout = subState.layout;
			newBeginning.range.aspectMask = aspectIntersection;
			newBeginning.range.baseArrayLayer = layerStartO;
			newBeginning.range.layerCount = layerEndO - layerStartO;
			newBeginning.range.baseMipLevel = subState.range.baseMipLevel;
			newBeginning.range.levelCount = levelStartO - subState.range.baseMipLevel;
		}
		if(subLevelEnd > changeLevelEnd) {
			auto& newEnd = newStates[numNewStates++];
			newEnd.layout = subState.layout;
			newEnd.range.aspectMask = aspectIntersection;
			newEnd.range.baseArrayLayer = layerStartO;
			newEnd.range.layerCount = layerEndO - layerStartO;
			newEnd.range.baseMipLevel = levelEndO;
			newEnd.range.levelCount = subLevelEnd - levelEndO;
		}

		if(subState.range.aspectMask == change.range.aspectMask) {
			// erase original state
			++inIt;
		} else {
			subState.range.aspectMask &= (~change.range.aspectMask);

			if(outIt != inIt) {
				*outIt = subState;
			}

			++outIt;
			++inIt;
		}
	}

	auto newSize = outIt - state.begin();
	newStates[numNewStates++] = change;

	return {newStates.first(numNewStates), newSize};
}

void checkForErrors(span<const ImageSubresourceLayout> state,
		const VkImageCreateInfo& ci) {
	// TODO: make sure covered range is consistent: continuous and rect-shaped

	auto allowedMask = aspects(ci.format);
	for(auto [i, sub] : enumerate(state)) {
		// check that sub is in bounds of ci
		dlg_assert((sub.range.aspectMask & ~allowedMask) == 0u);

		dlg_assert(sub.range.layerCount > 0);
		dlg_assert(sub.range.levelCount > 0);

		dlg_assert(sub.range.baseMipLevel + sub.range.levelCount <= ci.mipLevels);
		dlg_assert(sub.range.baseArrayLayer + sub.range.layerCount <= ci.arrayLayers);

		// check for non-overlap
		auto subLayerEnd = sub.range.baseArrayLayer + sub.range.layerCount;
		auto subLevelEnd = sub.range.baseMipLevel + sub.range.levelCount;
		for(auto [j, other] : enumerate(state)) {
			if(i == j) {
				continue;
			}

			if((other.range.aspectMask & sub.range.aspectMask) == 0u) {
				continue;
			}

			auto otherLayerEnd = other.range.baseArrayLayer + other.range.layerCount;
			auto otherLevelEnd = other.range.baseMipLevel + other.range.levelCount;

			auto layerStartO = std::max(
				sub.range.baseArrayLayer,
				other.range.baseArrayLayer);
			auto layerEndO = std::min(subLayerEnd, otherLayerEnd);
			auto levelStartO = std::max(
				sub.range.baseMipLevel,
				other.range.baseMipLevel);
			auto levelEndO = std::min(subLevelEnd, otherLevelEnd);

			dlg_assertm(layerEndO <= layerStartO || levelEndO <= levelStartO,
				"subresources {}, {} overlap in range: layers [{}, {}), mips [{}, {})",
				i, j, layerStartO, layerEndO, levelStartO, levelEndO);

		}
	}
}

VkImageLayout layout(span<const ImageSubresourceLayout> state,
		VkImageSubresource subres) {
	for(auto& sub : state) {
		if((sub.range.aspectMask & subres.aspectMask) &&
				subres.arrayLayer >= sub.range.baseArrayLayer &&
				subres.arrayLayer < sub.range.baseArrayLayer + sub.range.layerCount &&
				subres.mipLevel >= sub.range.baseMipLevel &&
				subres.mipLevel < sub.range.baseMipLevel + sub.range.levelCount) {

			dlg_assertm((sub.range.aspectMask & subres.aspectMask) == subres.aspectMask,
				"Specifying multiple aspects here isn't allowed");
			return sub.layout;
		}
	}

	dlg_error("subresource not found");
	return VK_IMAGE_LAYOUT_UNDEFINED;
}

void resolve(VkImageSubresourceRange& range, const VkImageCreateInfo& ci) {
	if(range.layerCount == VK_REMAINING_ARRAY_LAYERS) {
		range.layerCount = ci.arrayLayers - range.baseArrayLayer;
	}

	if(range.levelCount == VK_REMAINING_MIP_LEVELS) {
		range.levelCount = ci.mipLevels - range.baseMipLevel;
	}
}

} // namespace vil
