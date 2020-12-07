#include "memory.hpp"
#include "data.hpp"

namespace fuen {

// MemoryResource
MemoryResource::~MemoryResource() {
	if(!dev) {
		return;
	}

	// If this resource had memory assigned, remove it from the memories
	// list of resources.
	std::lock_guard lock(dev->mutex);
	if(memory) {
		dlg_assert(!memoryDestroyed);
		auto it = memory->allocations.find(this);
		dlg_assert(it != memory->allocations.end());
		memory->allocations.erase(it);
	}
}

// DeviceMemory
DeviceMemory::~DeviceMemory() {
	if(!dev) {
		return;
	}

	std::lock_guard lock(dev->mutex);
	for(auto* res : this->allocations) {
		dlg_assert(!res->memoryDestroyed);
		dlg_assert(res->memory == this);
		res->memory = nullptr;
		res->memoryDestroyed = true;
		res->allocationOffset = 0u;
		res->allocationSize = 0u;
	}
}

// Memory
VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(
		VkDevice                                    device,
		const VkMemoryAllocateInfo*                 pAllocateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkDeviceMemory*                             pMemory) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.AllocateMemory(device, pAllocateInfo, pAllocator, pMemory);
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
	dev.deviceMemories.mustErase(memory);
	dev.dispatch.FreeMemory(device, memory, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL MapMemory(
		VkDevice                                    device,
		VkDeviceMemory                              memory,
		VkDeviceSize                                offset,
		VkDeviceSize                                size,
		VkMemoryMapFlags                            flags,
		void**                                      ppData) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.MapMemory(device, memory, offset, size, flags, ppData);
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

	dev.dispatch.UnmapMemory(device, memory);
}

} // namespace fuen
