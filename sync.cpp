#include "sync.hpp"
#include "data.hpp"

namespace fuen {

VKAPI_ATTR VkResult VKAPI_CALL CreateFence(
		VkDevice                                    device,
		const VkFenceCreateInfo*                    pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkFence*                                    pFence) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.vkCreateFence(device, pCreateInfo, pAllocator, pFence);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& fence = dev.fences.add(*pFence);
	fence.objectType = VK_OBJECT_TYPE_FENCE;
	fence.dev = &dev;
	fence.handle = *pFence;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyFence(
		VkDevice                                    device,
		VkFence                                     fence,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	auto fenceD = dev.fences.mustMove(fence);

	// per spec, we can assume all associated payload to be finished
	{
		std::lock_guard lock(dev.mutex);
		if(fenceD->submission) {
			auto finished = checkLocked(*fenceD->submission);
			dlg_assert(finished);
		}

		// important that we do this while mutex is locked,
		// see ~DeviceHandle
		fenceD.reset();
	}

	dev.dispatch.vkDestroyFence(device, fence, pAllocator);
}

// We hook these calls so we have as much knowledge about payload
// completion as the application. Might make other things easier.

VKAPI_ATTR VkResult VKAPI_CALL ResetFences(
		VkDevice                                    device,
		uint32_t                                    fenceCount,
		const VkFence*                              pFences) {
	auto& dev = getData<Device>(device);

	// per spec, we can assume all associated payload to be finished
	for(auto i = 0u; i < fenceCount; ++i) {
		auto fence = pFences[i];
		auto& fenceD = dev.fences.get(fence);

		std::lock_guard lock(dev.mutex);
		if(fenceD.submission) {
			// per vulkan spec: fence must not be associated with a submission
			// that has not completed execution
			auto finished = checkLocked(*fenceD.submission);
			dlg_assert(finished);
		}
	}

	// technically, we could also just lock and reset on a per-fence basis
	MultiFenceLock lock(dev, {pFences, pFences + fenceCount});
	return dev.dispatch.vkResetFences(device, fenceCount, pFences);
}

VKAPI_ATTR VkResult VKAPI_CALL GetFenceStatus(
		VkDevice                                    device,
		VkFence                                     fence) {
	auto& dev = getData<Device>(device);
	auto& fenceD = dev.fences.get(fence);

	VkResult res;

	{
		std::lock_guard lock(fenceD.mutex);
		res = dev.dispatch.vkGetFenceStatus(device, fence);
	}

	if(res == VK_SUCCESS) {
		std::lock_guard lock(dev.mutex);
		if(fenceD.submission) {
			auto finished = checkLocked(*fenceD.submission);
			dlg_assert(finished);
		}
	}

	return res;
}

VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(
		VkDevice                                    device,
		uint32_t                                    fenceCount,
		const VkFence*                              pFences,
		VkBool32                                    waitAll,
		uint64_t                                    timeout) {
	auto& dev = getData<Device>(device);
	VkResult res;

	{
		MultiFenceLock lock(dev, {pFences, pFences + fenceCount});
		res = dev.dispatch.vkWaitForFences(device, fenceCount, pFences, waitAll, timeout);
	}

	if(res == VK_SUCCESS || !waitAll) {
		// have to check all fences for completed payloads
		for(auto i = 0u; i < fenceCount; ++i) {
			auto& fence = dev.fences.get(pFences[i]);
			std::lock_guard lock(dev.mutex);
			if(fence.submission) {
				checkLocked(*fence.submission);
			}
		}
	}

	return res;
}

// MultiFenceLock
MultiFenceLock::MultiFenceLock(Device& dev, span<const VkFence> fences) {
	std::vector<std::mutex*> mutexes;
	for(auto& fence : fences) {
		mutexes.push_back(&dev.fences.get(fence).mutex);
	}
	init(mutexes);
}

MultiFenceLock::MultiFenceLock(std::vector<std::mutex*> mutexes) {
	init(std::move(mutexes));
}

void MultiFenceLock::init(std::vector<std::mutex*> mutexes) {
	mutexes_ = std::move(mutexes);
	std::sort(mutexes_.begin(), mutexes_.end());
	for(auto* mtx : mutexes_) {
		mtx->lock();
	}
}

MultiFenceLock::~MultiFenceLock() {
	for(auto* mtx : mutexes_) {
		mtx->unlock();
	}
}

// semaphore
VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(
		VkDevice                                    device,
		const VkSemaphoreCreateInfo*                pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkSemaphore*                                pSemaphore) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.vkCreateSemaphore(device, pCreateInfo, pAllocator, pSemaphore);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& semaphore = dev.semaphores.add(*pSemaphore);
	semaphore.objectType = VK_OBJECT_TYPE_SEMAPHORE;
	semaphore.dev = &dev;
	semaphore.handle = *pSemaphore;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroySemaphore(
		VkDevice                                    device,
		VkSemaphore                                 semaphore,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.semaphores.mustErase(semaphore);
	dev.dispatch.vkDestroySemaphore(device, semaphore, pAllocator);
}

// event
VKAPI_ATTR VkResult VKAPI_CALL CreateEvent(
		VkDevice                                    device,
		const VkEventCreateInfo*                    pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkEvent*                                    pEvent) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.vkCreateEvent(device, pCreateInfo, pAllocator, pEvent);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& event = dev.events.add(*pEvent);
	event.objectType = VK_OBJECT_TYPE_EVENT;
	event.dev = &dev;
	event.handle = *pEvent;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyEvent(
		VkDevice                                    device,
		VkEvent                                     event,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.events.mustErase(event);
	dev.dispatch.vkDestroyEvent(device, event, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL GetEventStatus(
		VkDevice                                    device,
		VkEvent                                     event) {
	auto& dev = getData<Device>(device);
	return dev.dispatch.vkGetEventStatus(device, event);
}

VKAPI_ATTR VkResult VKAPI_CALL SetEvent(
		VkDevice                                    device,
		VkEvent                                     event) {
	auto& dev = getData<Device>(device);
	return dev.dispatch.vkSetEvent(device, event);
}

VKAPI_ATTR VkResult VKAPI_CALL ResetEvent(
		VkDevice                                    device,
		VkEvent                                     event) {
	auto& dev = getData<Device>(device);
	return dev.dispatch.vkResetEvent(device, event);
}

} // namespace fuen
