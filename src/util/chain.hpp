#pragma once

#include <fwd.hpp>
#include <vk/vulkan_core.h>
#include <memory>
#include <functional>

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

// TODO: just optionally properly do deep copies

// When given, called with each copied chain element. Can freely
// modify it. When it returns false, the chain element is omitted from
// the copied chain.
using ChainFunc = std::function<bool(VkBaseOutStructure&)>;
using ChainMem = std::unique_ptr<std::byte[]>;

[[nodiscard]] ChainMem copyChain(const void* pNext, const ChainFunc& = {});
[[nodiscard]] inline ChainMem copyChainReplace(const void*& pNext, const ChainFunc& = {}) {
	auto ret = copyChain(pNext);
	pNext = ret.get();
	return ret;
}

void* copyChainReplace(const void*& pNext, ChainMem& buf, const ChainFunc& = {});
void* copyChainLocal(LinAllocScope&, const void* pNext, const ChainFunc& = {});
void* copyChain(LinAllocator&, const void* pNext, const ChainFunc& = {});

} // namespace vil
