#pragma once

#include "common.hpp"
#include <set>

namespace fuen {

// TODO: support sparse binding
struct MemoryResource {
	DeviceMemory* memory {};
	VkDeviceSize allocationOffset {};
	VkDeviceSize allocationSize {};

	MemoryResource() = default;
	MemoryResource(const Buffer&) = delete;
	MemoryResource& operator=(const MemoryResource&) = delete;
	~MemoryResource();
};

struct DeviceMemory {
	Device* dev;
	VkDeviceMemory handle;
	std::string name;

	u32 typeIndex;
	VkDeviceSize size;

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

	DeviceMemory() = default;
	DeviceMemory(const DeviceMemory&) = delete;
	DeviceMemory& operator=(const DeviceMemory&) = delete;
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
