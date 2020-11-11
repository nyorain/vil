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

} // namespace fuen
