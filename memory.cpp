#include "memory.hpp"
#include "data.hpp"

namespace fuen {

// MemoryResource
MemoryResource::~MemoryResource() {
	if(memory) {
		for(auto it = memory->allocations.begin(); it != memory->allocations.end();) {
			if(it->resource == this) {
				dlg_assert(it->offset == allocationOffset);
				dlg_assert(it->size == allocationSize);
				it = memory->allocations.erase(it);
				continue;
			}

			++it;
		}
	}
}

// Memory
VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(
		VkDevice                                    device,
		const VkMemoryAllocateInfo*                 pAllocateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkDeviceMemory*                             pMemory) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.vkAllocateMemory(device, pAllocateInfo, pAllocator, pMemory);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& memory = dev.deviceMemories.add(*pMemory);
	memory.objectType = VK_OBJECT_TYPE_DEVICE_MEMORY;
	memory.dev = &dev;
	memory.handle = *pMemory;
	memory.typeIndex = pAllocateInfo->memoryTypeIndex;
	memory.size = pAllocateInfo->allocationSize;

	return res;
}

VKAPI_ATTR void VKAPI_CALL FreeMemory(
		VkDevice                                    device,
		VkDeviceMemory                              memory,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	{
		auto mem = dev.deviceMemories.mustMove(memory);
		dlg_assert(mem->allocations.empty());
	}
	dev.dispatch.vkFreeMemory(device, memory, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL MapMemory(
		VkDevice                                    device,
		VkDeviceMemory                              memory,
		VkDeviceSize                                offset,
		VkDeviceSize                                size,
		VkMemoryMapFlags                            flags,
		void**                                      ppData) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.vkMapMemory(device, memory, offset, size, flags, ppData);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& mem = dev.deviceMemories.get(memory);
	mem.map = *ppData;
	mem.mapOffset = offset;
	mem.mapSize = size;

	return res;
}

VKAPI_ATTR void VKAPI_CALL UnmapMemory(
		VkDevice                                    device,
		VkDeviceMemory                              memory) {
	auto& dev = getData<Device>(device);
	auto& mem = dev.deviceMemories.get(memory);

	mem.map = nullptr;
	mem.mapOffset = 0u;
	mem.mapSize = 0u;

	dev.dispatch.vkUnmapMemory(device, memory);
}

} // namespace fuen
