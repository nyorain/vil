#include <memory.hpp>
#include <device.hpp>
#include <data.hpp>
#include <image.hpp>
#include <buffer.hpp>
#include <threadContext.hpp>
#include <util/util.hpp>

namespace vil {

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
		auto it = find(memory->allocations, this);
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

	// dlg_info("Destroying DeviceMemory {} ({})", this, handle);

	// Since we modify the elements inside the sets (with respect to
	// our comparison iterator) we can't just iterate through them.
	// Remove and reset one-by-one instead.
	// TODO: nvm, fix for new vector-based allocations
	while(!this->allocations.empty())
	{
		// auto* res = *this->allocations.begin();
		// this->allocations.erase(this->allocations.begin());
		auto* res = this->allocations.back();
		this->allocations.pop_back();

		// dlg_info("  Unregister handle {}", res);

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
	auto& dev = getDevice(device);

	ThreadMemScope memScope;
	auto chainCopy = copyChainLocal(memScope, pAllocateInfo->pNext);
	auto allocInfo = *pAllocateInfo;
	allocInfo.pNext = chainCopy;

	// unwrap handles in VkMemoryDedicatedAllocateInfo
	auto* dedicatedv = findChainInfo2<VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO>(chainCopy);
	if(dedicatedv) {
		auto* dedicated = static_cast<VkMemoryDedicatedAllocateInfo*>(dedicatedv);
		dlg_assert(!dedicated->buffer || !dedicated->image);
		if(dedicated->buffer) {
			auto& buf = get(dev, dedicated->buffer);
			dedicated->buffer = buf.handle;
		} else if(dedicated->image) {
			auto& img = get(dev, dedicated->image);
			dedicated->image = img.handle;
		}
	}

	auto res = dev.dispatch.AllocateMemory(dev.handle, &allocInfo, pAllocator, pMemory);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto memPtr = std::make_unique<DeviceMemory>();
	auto& memory = *memPtr;
	memory.objectType = VK_OBJECT_TYPE_DEVICE_MEMORY;
	memory.dev = &dev;
	memory.handle = *pMemory;
	memory.typeIndex = pAllocateInfo->memoryTypeIndex;
	memory.size = pAllocateInfo->allocationSize;

	*pMemory = castDispatch<VkDeviceMemory>(memory);
	dev.deviceMemories.mustEmplace(memory.handle, std::move(memPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL FreeMemory(
		VkDevice                                    device,
		VkDeviceMemory                              memory,
		const VkAllocationCallbacks*                pAllocator) {
	if(!memory) {
		return;
	}

	auto& dev = getDevice(device);
	auto handle = dev.deviceMemories.mustMove(memory)->handle;
	dev.dispatch.FreeMemory(dev.handle, handle, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL MapMemory(
		VkDevice                                    device,
		VkDeviceMemory                              memory,
		VkDeviceSize                                offset,
		VkDeviceSize                                size,
		VkMemoryMapFlags                            flags,
		void**                                      ppData) {
	auto& dev = getDevice(device);
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
	auto& dev = getDevice(device);
	auto& mem = dev.deviceMemories.get(memory);

	mem.map = nullptr;
	mem.mapOffset = 0u;
	mem.mapSize = 0u;

	dev.dispatch.UnmapMemory(device, memory);
}

} // namespace vil
