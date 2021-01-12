#pragma once

#include <fwd.hpp>
#include <memory.hpp>
#include <unordered_set>

namespace fuen {

struct Buffer : MemoryResource {
	VkBuffer handle {};
	VkBufferCreateInfo ci;

	std::vector<BufferView*> views; // TODO: unordered_set?
	std::unordered_set<DescriptorSetRef, DescriptorSetRef::Hash> descriptors;

	bool concurrentHooked {}; // whether we set it to concurrent mode

	~Buffer();
};

struct BufferView : DeviceHandle {
	VkBufferView handle;
	VkBufferViewCreateInfo ci;
	Buffer* buffer {};

	std::unordered_set<DescriptorSetRef, DescriptorSetRef::Hash> descriptors;

	~BufferView();
};

VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(
    VkDevice                                    device,
    const VkBufferCreateInfo*                   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkBuffer*                                   pBuffer);

VKAPI_ATTR void VKAPI_CALL DestroyBuffer(
    VkDevice                                    device,
    VkBuffer                                    buffer,
    const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(
    VkDevice                                    device,
    VkBuffer                                    buffer,
    VkDeviceMemory                              memory,
    VkDeviceSize                                memoryOffset);

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2(
    VkDevice                                    device,
    uint32_t                                    bindInfoCount,
    const VkBindBufferMemoryInfo*               pBindInfos);

VKAPI_ATTR VkResult VKAPI_CALL CreateBufferView(
    VkDevice                                    device,
    const VkBufferViewCreateInfo*               pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkBufferView*                               pView);

VKAPI_ATTR void VKAPI_CALL DestroyBufferView(
    VkDevice                                    device,
    VkBufferView                                bufferView,
    const VkAllocationCallbacks*                pAllocator);

} // namespace fuen
