#include "buffer.hpp"
#include "data.hpp"

namespace fuen {

VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(
		VkDevice                                    device,
		const VkBufferCreateInfo*                   pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkBuffer*                                   pBuffer) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.vkCreateBuffer(device, pCreateInfo, pAllocator, pBuffer);
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
	dev.dispatch.vkDestroyBuffer(device, buffer, pAllocator);
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
	dev.dispatch.vkGetBufferMemoryRequirements(device, buffer, &memReqs);

	buf.memory = &mem;
	buf.allocationOffset = memoryOffset;
	buf.allocationSize = memReqs.size;
	mem.allocations.insert({memoryOffset, memReqs.size, &buf});

	return dev.dispatch.vkBindBufferMemory(device, buffer, memory, memoryOffset);
}

} // namespace fuen
