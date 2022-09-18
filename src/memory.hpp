#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <util/dlg.hpp>
#include <set>

// TODO: support sparse binding

namespace vil {

struct MemoryResource : SharedDeviceHandle {
	enum class State {
		unbound,
		bound,
		memoryDestroyed,
		resourceDestroyed,
	};

	State memState {State::unbound};
	DeviceMemory* memory {};
	VkDeviceSize allocationOffset {};
	VkDeviceSize allocationSize {};
	VkObjectType memObjectType {};

	void onApiDestroy();
};

struct DeviceMemory : SharedDeviceHandle {
	static constexpr auto objectType = VK_OBJECT_TYPE_DEVICE_MEMORY;

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

	void onApiDestroy();
};

// Returns whether any parts of the given memory resource alias any
// other resource.
// Expects the given MemoryResource to be bound to memory.
// Expects the device mutex to be locked.
bool aliasesOtherResourceLocked(const MemoryResource& res);

// Returns whether the given memory resources lies in currently mapped memory.
// Expects the given MemoryResource to be bound to memory.
// Expects the device mutex to be locked.
bool currentlyMappedLocked(const MemoryResource& res);

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
