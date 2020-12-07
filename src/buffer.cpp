#include "buffer.hpp"
#include "data.hpp"
#include "ds.hpp"

namespace fuen {

// Classes
Buffer::~Buffer() {
	if(!dev) {
		return;
	}

	std::lock_guard lock(dev->mutex);

	// Don't use for-loop as the descriptors unregister themselves
	while(!this->descriptors.empty()) {
		auto ref = this->descriptors[0];
		dlg_assert(ref.ds->getBuffer(ref.binding, ref.elem) == this);
		ref.ds->invalidateLocked(ref.binding, ref.elem);
	}
}

// API
VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(
		VkDevice                                    device,
		const VkBufferCreateInfo*                   pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkBuffer*                                   pBuffer) {
	auto& dev = getData<Device>(device);

	auto nci = *pCreateInfo;
	// TODO: needed for our own operations on the buffer. We should
	// properly acquire/release it instead though, this might have
	// a performance impact.
	nci.sharingMode = VK_SHARING_MODE_CONCURRENT;

	auto res = dev.dispatch.CreateBuffer(device, &nci, pAllocator, pBuffer);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& buf = dev.buffers.add(*pBuffer);
	buf.objectType = VK_OBJECT_TYPE_BUFFER;
	buf.dev = &dev;
	buf.ci = *pCreateInfo;
	buf.handle = *pBuffer;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyBuffer(
		VkDevice                                    device,
		VkBuffer                                    buffer,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.buffers.mustErase(buffer);
	dev.dispatch.DestroyBuffer(device, buffer, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(
		VkDevice                                    device,
		VkBuffer                                    buffer,
		VkDeviceMemory                              memory,
		VkDeviceSize                                memoryOffset) {
	auto& dev = getData<Device>(device);
	auto& buf = dev.buffers.get(buffer);
	auto& mem = dev.deviceMemories.get(memory);

	dlg_assert(!buf.memory);

	// find required size
	VkMemoryRequirements memReqs;
	dev.dispatch.GetBufferMemoryRequirements(device, buffer, &memReqs);

	buf.memory = &mem;
	buf.allocationOffset = memoryOffset;
	buf.allocationSize = memReqs.size;

	{
		// access to the given memory must be internally synced
		std::lock_guard lock(dev.mutex);
		mem.allocations.insert(&buf);
	}

	return dev.dispatch.BindBufferMemory(device, buffer, memory, memoryOffset);
}

} // namespace fuen
