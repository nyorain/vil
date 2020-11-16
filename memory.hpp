#pragma once

#include "device.hpp"
#include <set>

namespace fuen {

// TODO: support sparse binding
struct MemoryResource : DeviceHandle {
	enum class Type {
		buffer,
		image,
	};

	DeviceMemory* memory {};
	VkDeviceSize allocationOffset {};
	VkDeviceSize allocationSize {};
	Type memoryResourceType {};

	~MemoryResource();
};

struct DeviceMemory : DeviceHandle {
	VkDeviceMemory handle {};

	u32 typeIndex {};
	VkDeviceSize size {};

	struct Allocation {
		VkDeviceSize offset;
		VkDeviceSize size;
		MemoryResource* resource;
	};

	struct AllocationCmp {
		bool operator()(const Allocation& a, const Allocation& b) const noexcept {
			return a.offset < b.offset;
		}
	};

	std::set<Allocation, AllocationCmp> allocations;
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

} // namespace fuen
