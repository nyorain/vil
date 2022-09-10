#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <atomic>

namespace vil {

struct Fence : SharedDeviceHandle {
	VkFence handle {};

	// The pending submission this fence is currently associated to.
	// Synced via device mutex.
	SubmissionBatch* submission {};

	Fence() = default;
	~Fence();
	Fence(Fence&&) = delete;
	Fence& operator=(Fence&&) = delete;
};

struct Semaphore : SharedDeviceHandle {
	VkSemaphore handle {};

	struct Sync {
		Submission* submission {};
		VkPipelineStageFlags stages {};
		u64 value {1u}; // always 1 for binaries
	};

	std::vector<Sync> signals;
	std::vector<Sync> waits;

	VkSemaphoreType type {};

	// Lower bound of the value. We know that the current value of
	// the semaphore is at least this.
	u64 lowerBound {};

	// Upper bound of the value. We know that the current value of
	// the semaphoer can't be greater.
	u64 upperBound {};

	Semaphore() = default;
	~Semaphore();
};

// Updates upperBound
void updateUpperLocked(Semaphore& sem, u64 value);

struct Event : SharedDeviceHandle {
	VkEvent handle {};
	~Event();
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

VKAPI_ATTR VkResult VKAPI_CALL SignalSemaphore(
    VkDevice                                    device,
    const VkSemaphoreSignalInfo*                pSignalInfo);

VKAPI_ATTR VkResult VKAPI_CALL WaitSemaphores(
    VkDevice                                    device,
    const VkSemaphoreWaitInfo*                  pWaitInfo,
    uint64_t                                    timeout);

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
