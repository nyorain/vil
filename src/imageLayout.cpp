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

bool simplify0(std::vector<ImageSubresourceLayout>& state) {
	auto changed = false;
	for(auto it = state.begin(); it != state.end();) {
		auto& sub = *it;
		if(sub.range.baseArrayLayer == 0 && sub.range.baseMipLevel == 0) {
			++it;
			continue;
		}

		auto erase = false;
		auto subLevelEnd = sub.range.baseMipLevel + sub.range.levelCount;
		auto subLayerEnd = sub.range.baseArrayLayer + sub.range.layerCount;
		for(auto& other : state) {
			if(other.range.aspectMask != sub.range.aspectMask ||
					other.layout != sub.layout) {
				continue;
			}

			auto otherLayerEnd = other.range.baseArrayLayer + other.range.layerCount;
			auto otherLevelEnd = other.range.baseMipLevel + other.range.levelCount;

			// greedy
			// lower layers
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
				changed = true;

				if(other.range.baseMipLevel == sub.range.baseMipLevel &&
						other.range.levelCount == sub.range.levelCount) {
					erase = true;
				} else if(other.range.baseMipLevel != sub.range.baseMipLevel) {
					// shrink to just beginning
					sub.range.levelCount = other.range.baseMipLevel - sub.range.baseMipLevel;
				} else {
					// shrink to just end
					sub.range.levelCount = subLevelEnd - otherLevelEnd;
					sub.range.baseMipLevel = otherLevelEnd;
				}
			}

			// higher layers
			if(subLayerEnd == other.range.baseArrayLayer &&
					// 'sub' covers at least the mip range of 'other'
					other.range.baseMipLevel >= sub.range.baseMipLevel &&
					otherLevelEnd <= subLevelEnd &&
					// make sure that 'sub' is at one end of the mip range
					// of 'other' to avoid inserting additional states
					(other.range.baseMipLevel == sub.range.baseMipLevel ||
					otherLevelEnd == subLevelEnd)) {

				// can merge!
				other.range.layerCount += sub.range.layerCount;
				other.range.baseArrayLayer = sub.range.baseArrayLayer;
				changed = true;

				if(other.range.baseMipLevel == sub.range.baseMipLevel &&
						other.range.levelCount == sub.range.levelCount) {
					erase = true;
				} else if(other.range.baseMipLevel != sub.range.baseMipLevel) {
					// shrink to just beginning
					sub.range.levelCount = other.range.baseMipLevel - sub.range.baseMipLevel;
				} else {
					// shrink to just end
					sub.range.levelCount = subLevelEnd - otherLevelEnd;
					sub.range.baseMipLevel = otherLevelEnd;
				}
			}
		}

		if(erase) {
			it = state.erase(it);
		} else {
			++it;
		}
	}

	return changed;
}

bool simplify1(std::vector<ImageSubresourceLayout>& state) {
	auto changed = false;
	for(auto it = state.begin(); it != state.end();) {
		auto& sub = *it;
		if(sub.range.baseArrayLayer == 0 && sub.range.baseMipLevel == 0) {
			++it;
			continue;
		}

		auto erase = false;
		for(auto& other : state) {
			if(other.range.aspectMask != sub.range.aspectMask ||
					other.layout != sub.layout) {
				continue;
			}

			auto otherLevelEnd = other.range.baseMipLevel + other.range.levelCount;

			if(sub.range.baseMipLevel > 0 &&
					otherLevelEnd == sub.range.baseMipLevel &&
					other.range.baseArrayLayer == sub.range.baseArrayLayer &&
					other.range.layerCount == sub.range.layerCount) {
				// can merge!
				other.range.levelCount += sub.range.levelCount;
				changed = true;
				erase = true;
				break;
			}

		}

		if(erase) {
			it = state.erase(it);
		} else {
			++it;
		}
	}

	return changed;
}

void simplify(std::vector<ImageSubresourceLayout>& state) {
	// while(simplify0(state));
	// while(simplify1(state));
	// return;

	for(auto it = state.begin(); it != state.end();) {
		auto& sub = *it;
		if(sub.range.baseArrayLayer == 0 && sub.range.baseMipLevel == 0) {
			++it;
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

			// greedy
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
			it = state.erase(it);
		} else {
			++it;
		}
	}

	// TODO: also simplify for aspect masks
	// TODO: in certain situations we won't simplify even though
	//   it's possible (when levelCount/layerCount is never equal).
	//   Might be able to fix it by greedily merging in one
	//   dimension first (e.g. mips)
}

void apply(std::vector<ImageSubresourceLayout>& state,
		const ImageSubresourceLayout& change) {

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
		return);

	// TODO: fast path for *full* overlap with a state

	// cut out 'change.range' from 'range' in all 'state' elements
	auto changeLayerEnd = change.range.baseArrayLayer + change.range.layerCount;
	auto changeLevelEnd = change.range.baseMipLevel + change.range.levelCount;

	dlg_assertlm(dlg_level_warn, state.size() < 1024u,
		"Large ImageLayout state detected");

	ThreadMemScope tms;
	auto newStates = tms.alloc<ImageSubresourceLayout>(state.size() * 4);
	auto numNewStates = 0u;

	for(auto it = state.begin(); it != state.end();) {
		auto subState = *it;
		auto aspectIntersection = subState.range.aspectMask & change.range.aspectMask;
		if(aspectIntersection == 0u) {
			++it;
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

		if(layerEndO <= layerStartO || levelEndO <= levelStartO) {
			++it;
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

		// erase original state
		if(subState.range.aspectMask == change.range.aspectMask) {
			it = state.erase(it);
		} else {
			subState.range.aspectMask &= (~change.range.aspectMask);
			++it;
		}
	}

	state.insert(state.end(), newStates.begin(), newStates.begin() + numNewStates);
	state.push_back(change);
}

void apply(std::vector<ImageSubresourceLayout>& state,
		span<const ImageSubresourceLayout> changes) {
	for(auto& change : changes) {
		apply(state, change);
	}

	// TODO: simplify here?
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

} // namespace vil
