#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <set>

namespace fuen {

// TODO: support sparse binding
struct MemoryResource : DeviceHandle {
	// !memoryDestroyed && !memory: no memory associated
	// !memoryDestroyed && memory: memory associated
	// memoryDestroyed && memory: invalid state, must not exist
	// memoryDestroyed && !memory: had memory associated but memory object was destroyed
	bool memoryDestroyed {};
	DeviceMemory* memory {};
	VkDeviceSize allocationOffset {};
	VkDeviceSize allocationSize {};

	~MemoryResource();
};

struct DeviceMemory : DeviceHandle {
	VkDeviceMemory handle {};

	u32 typeIndex {};
	VkDeviceSize size {};

	void* map {}; // nullptr if not mapped
	VkDeviceSize mapOffset {};
	VkDeviceSize mapSize {};

	// NOTE: we can't use a set since resources may alias
	// struct AllocationCmp {
	// 	bool operator()(const MemoryResource* a, const MemoryResource* b) const noexcept {
	// 		dlg_assertm(a == b ||
	// 			a->allocationOffset + a->allocationSize <= b->allocationOffset ||
	// 			b->allocationOffset + b->allocationSize <= a->allocationOffset,
	// 			"{} {} vs {} {}", a->allocationOffset, a->allocationSize, b->allocationOffset, b->allocationSize);
	// 		return a->allocationOffset < b->allocationOffset;
	// 	}
	// };
	// std::set<MemoryResource*, AllocationCmp> allocations;
	std::vector<MemoryResource*> allocations; // TODO: unsorted, should be sorted for viz

	~DeviceMemory();
};

VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(
    VkDevice                                    device,
    const VkMemoryAllocateInfo*                 pAllocateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDeviceMemory*                             pMemory);

VKAPI_ATTR void VKAPI_CALL FreeMemory(
    VkDevice                                    device,
    VkDeviceMemory                              memory,
    const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL MapMemory(
    VkDevice                                    device,
    VkDeviceMemory                              memory,
    VkDeviceSize                                offset,
    VkDeviceSize                                size,
    VkMemoryMapFlags                            flags,
    void**                                      ppData);

VKAPI_ATTR void VKAPI_CALL UnmapMemory(
    VkDevice                                    device,
    VkDeviceMemory                              memory);

} // namespace fuen
