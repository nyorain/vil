#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <set>

namespace vil {

// TODO: support sparse binding
struct MemoryResource : DeviceHandle {
	// !memoryDestroyed && !memory: no memory associated
	// !memoryDestroyed && memory: memory associated
	// memoryDestroyed && !memory: had memory associated but memory object was destroyed
	// memoryDestroyed && memory: invalid state, must not exist
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

	// Describes the current map state
	void* map {}; // nullptr if not mapped
	VkDeviceSize mapOffset {};
	VkDeviceSize mapSize {};

	// Sorted by offset. Keep in mind that allocations may alias. If
	// multiple allocations have the same offset, the sorting between those
	// is arbitrary.
	std::vector<MemoryResource*> allocations;

	~DeviceMemory();
};

// Used as comparator to std::lower_bound when searching for an allocation
// in DeviceMemory::allocations.
inline bool cmpMemRes(const MemoryResource* it, const MemoryResource& b) {
	dlg_assert(it->memory == b.memory);
	return it->allocationOffset < b.allocationOffset;
}

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

VKAPI_ATTR VkResult VKAPI_CALL FlushMappedMemoryRanges(
    VkDevice                                    device,
    uint32_t                                    memoryRangeCount,
    const VkMappedMemoryRange*                  pMemoryRanges);

VKAPI_ATTR VkResult VKAPI_CALL InvalidateMappedMemoryRanges(
    VkDevice                                    device,
    uint32_t                                    memoryRangeCount,
    const VkMappedMemoryRange*                  pMemoryRanges);

VKAPI_ATTR void VKAPI_CALL GetDeviceMemoryCommitment(
    VkDevice                                    device,
    VkDeviceMemory                              memory,
    VkDeviceSize*                               pCommittedMemoryInBytes);

} // namespace vil
