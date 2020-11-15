#pragma once

#include "common.hpp"
#include <atomic>

namespace fuen {

struct Fence {
	Device* dev {};
	VkFence fence {};
	std::string name;

	PendingSubmission* submission {};

	// Needed since we might have to wait on a fence internally
	// and must synchronize that with fence waits from the application
	// (in other threads). So this mutex must be locked while the fence
	// is in a call down the chain.
	// NOTE: not necessarily locked when members are changed.
	std::mutex mutex {};
};

// api
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

// Locks multiple fences.
// Always call this instead of just locking them in any order to avoid
// deadlocks between multiple mult-fence-locks.
struct MultiFenceLock {
	MultiFenceLock(Device& dev, span<const VkFence>);
	MultiFenceLock(std::vector<std::mutex*>);
	~MultiFenceLock();

private:
	void init(std::vector<std::mutex*>);
	std::vector<std::mutex*> mutexes_;
};

} // namespace fuen
