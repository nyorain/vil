#pragma once

#include <fwd.hpp>
#include <memory.hpp>
#include <unordered_set>
#include <atomic>

namespace vil {

struct Buffer : MemoryResource {
	static constexpr auto objectType = VK_OBJECT_TYPE_BUFFER;

	VkBuffer handle {};
	VkBufferCreateInfo ci;
	bool concurrentHooked {}; // whether we set it to concurrent mode
	bool externalMemory {}; // whether it supports importing/exporting

	std::vector<BufferView*> views;
	VkDeviceAddress deviceAddress {}; // null if not available

	~Buffer();
	void onApiDestroy();
};

struct BufferView : SharedDeviceHandle {
	static constexpr auto objectType = VK_OBJECT_TYPE_BUFFER_VIEW;

	VkBufferView handle;
	VkBufferViewCreateInfo ci;
	Buffer* buffer {}; // TODO: IntrusivePtr?

	~BufferView();
};

// util
// When range isn't VK_WHOLE_SIZE, simply returns range.
// Otherwise returns fullSize - offset.
VkDeviceSize evalRange(VkDeviceSize fullSize, VkDeviceSize offset, VkDeviceSize range);

// Returns the buffer associated with the given address (does not have to
// be the base address, can be somewhere inside the buffer).
// If there are multiple buffers with overlapping addresses (can happen
// e.g. with memory aliasing I guess), will return the one that contains the
// largest range from the given address.
// Will lock the device mutex internally.
Buffer& bufferAt(Device& dev, VkDeviceAddress address);
Buffer& bufferAtLocked(Device& dev, VkDeviceAddress address);

// API
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

VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements(
    VkDevice                                    device,
    VkBuffer                                    buffer,
    VkMemoryRequirements*                       pMemoryRequirements);

VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements2(
    VkDevice                                    device,
    const VkBufferMemoryRequirementsInfo2*      pInfo,
    VkMemoryRequirements2*                      pMemoryRequirements);

VKAPI_ATTR VkResult VKAPI_CALL CreateBufferView(
    VkDevice                                    device,
    const VkBufferViewCreateInfo*               pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkBufferView*                               pView);

VKAPI_ATTR void VKAPI_CALL DestroyBufferView(
    VkDevice                                    device,
    VkBufferView                                bufferView,
    const VkAllocationCallbacks*                pAllocator);

// VK_KHR_buffer_device_address
VKAPI_ATTR VkDeviceAddress VKAPI_CALL GetBufferDeviceAddress(
    VkDevice                                    device,
    const VkBufferDeviceAddressInfo*            pInfo);

VKAPI_ATTR uint64_t VKAPI_CALL GetBufferOpaqueCaptureAddress(
    VkDevice                                    device,
    const VkBufferDeviceAddressInfo*            pInfo);

VKAPI_ATTR uint64_t VKAPI_CALL GetDeviceMemoryOpaqueCaptureAddress(
    VkDevice                                    device,
    const VkDeviceMemoryOpaqueCaptureAddressInfo* pInfo);

} // namespace vil
