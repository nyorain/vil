#pragma once

#include <vk/vulkan_core.h>
#include <memory>
#include <fwd.hpp>

namespace vil {

template<typename Base, typename Link>
void addChain(Base& base, Link& link) {
	link.pNext = const_cast<void*>(base.pNext);
	base.pNext = &link;
}

template<typename CI>
bool hasChain(const CI& ci, VkStructureType sType) {
	auto* link = static_cast<const VkBaseInStructure*>(ci.pNext);
	while(link) {
		if(link->sType == sType) {
			return true;
		}

		link = static_cast<const VkBaseInStructure*>(link->pNext);
	}

	return false;
}

template<typename R, VkStructureType SType, typename CI>
const R* findChainInfo(const CI& ci) {
	auto* link = static_cast<const VkBaseInStructure*>(ci.pNext);
	while(link) {
		if(link->sType == SType) {
			return reinterpret_cast<const R*>(link);
		}

		link = static_cast<const VkBaseInStructure*>(link->pNext);
	}

	return nullptr;
}

template<VkStructureType SType>
const void* findChainInfo2(const void* pNext) {
	auto* link = static_cast<const VkBaseInStructure*>(pNext);
	while(link) {
		if(link->sType == SType) {
			return static_cast<const void*>(link);
		}

		link = static_cast<const VkBaseInStructure*>(link->pNext);
	}

	return nullptr;
}

template<VkStructureType SType>
void* findChainInfo2(void* pNext) {
	auto* link = static_cast<VkBaseOutStructure*>(pNext);
	while(link) {
		if(link->sType == SType) {
			return static_cast<void*>(link);
		}

		link = static_cast<VkBaseOutStructure*>(link->pNext);
	}

	return nullptr;
}

// IDEA: integrate unwrapping into this?

std::unique_ptr<std::byte[]> copyChain(const void* pNext);
inline std::unique_ptr<std::byte[]> copyChainPatch(const void*& pNext) {
	auto ret = copyChain(pNext);
	pNext = ret.get();
	return ret;
}

void* copyChainPatch(const void*& pNext, std::unique_ptr<std::byte[]>& buf);

void* copyChainLocal(LinAllocScope&, const void* pNext);
void* copyChain(LinAllocator&, const void* pNext);

} // namespace vil
