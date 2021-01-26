#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <atomic>

namespace vil {

struct Fence : DeviceHandle {
	VkFence handle {};

	// The pending submission this fence is currently associated to.
	// Synced via device mutex.
	SubmissionBatch* submission {};

	~Fence();
};

struct Semaphore : DeviceHandle {
	VkSemaphore handle {};

	// NOTE: eventually, we'll want to support other kinds of payloads here.
	// Can be signaled/waited upon in QueuePresent, AcquireImage and more.
    // NOTE: it's not that easy to represent these chains. Semaphores can have
    // multiple wait and signal operations pending as long as they are guaranteed
    // to happen strictly in signal->wait, signal->wait, ... order.
    // We can probably detect at submission time which signal belongs to which wait,
    // represent it via new "SemaphoreLink" struct or something and allow to store
    // multiple ocurrences of that hrere? -> submission rework/display
	// SubmissionBatch* signalFrom {};
	// SubmissionBatch* waitFrom {};

	~Semaphore();
};

struct Event : DeviceHandle {
	VkEvent handle {};
};

// api
// fence
VKAPI_ATTR VkResult VKAPI_CALL CreateFence(
    VkDevice                                    device,
    const VkFenceCreateInfo*                    pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkFence*                                    pFence);

VKAPI_ATTR void VKAPI_CALL DestroyFence(
    VkDevice                                    device,
    VkFence                                     fence,
    const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL ResetFences(
    VkDevice                                    device,
    uint32_t                                    fenceCount,
    const VkFence*                              pFences);

VKAPI_ATTR VkResult VKAPI_CALL GetFenceStatus(
    VkDevice                                    device,
    VkFence                                     fence);

VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(
    VkDevice                                    device,
    uint32_t                                    fenceCount,
    const VkFence*                              pFences,
    VkBool32                                    waitAll,
    uint64_t                                    timeout);

// semaphore
VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(
    VkDevice                                    device,
    const VkSemaphoreCreateInfo*                pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSemaphore*                                pSemaphore);

VKAPI_ATTR void VKAPI_CALL DestroySemaphore(
    VkDevice                                    device,
    VkSemaphore                                 semaphore,
    const VkAllocationCallbacks*                pAllocator);

// event
VKAPI_ATTR VkResult VKAPI_CALL CreateEvent(
    VkDevice                                    device,
    const VkEventCreateInfo*                    pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkEvent*                                    pEvent);

VKAPI_ATTR void VKAPI_CALL DestroyEvent(
    VkDevice                                    device,
    VkEvent                                     event,
    const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL GetEventStatus(
    VkDevice                                    device,
    VkEvent                                     event);

VKAPI_ATTR VkResult VKAPI_CALL SetEvent(
    VkDevice                                    device,
    VkEvent                                     event);

VKAPI_ATTR VkResult VKAPI_CALL ResetEvent(
    VkDevice                                    device,
    VkEvent                                     event);

} // namespace vil
