#include <sync.hpp>
#include <wrap.hpp>
#include <device.hpp>
#include <data.hpp>
#include <queue.hpp>
#include <util/util.hpp>

namespace vil {

Fence::~Fence() {
	if(!dev) {
		return;
	}

	// per spec, we can assume all associated payload to be finished
	std::lock_guard lock(dev->mutex);
	if(this->submission) {
		auto finished = checkLocked(*this->submission);
		dlg_assert(finished);
	}
}

Semaphore::~Semaphore() {
	if(!dev) {
		return;
	}

	// per spec, we can assume all associated payload to be finished
	/*
	std::lock_guard lock(dev->mutex);
	if(this->signalFrom) {
		auto finished = checkLocked(*this->signalFrom);
		dlg_assert(finished);
	}

	if(this->waitFrom) {
		auto finished = checkLocked(*this->waitFrom);
		dlg_assert(finished);
	}
	*/
}

VKAPI_ATTR VkResult VKAPI_CALL CreateFence(
		VkDevice                                    device,
		const VkFenceCreateInfo*                    pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkFence*                                    pFence) {
	auto& dev = getDevice(device);
	auto res = dev.dispatch.CreateFence(device, pCreateInfo, pAllocator, pFence);
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
	if(!fence) {
		return;
	}

	auto& dev = getDevice(device);
	dev.fences.mustErase(fence);
	dev.dispatch.DestroyFence(device, fence, pAllocator);
}

// We hook these calls so we have as much knowledge about payload
// completion as the application. Might make other things easier.
// NOTE: For vulkan, ResetFence calls must be synchronized but otherwise,
// multiple threads can call wait/getFenceStatus at the same time.
// That's why we don't have to synchronize these calls even though we might
// call getFenceStatus on application fences (that have a pending submission)
// from other places.

VKAPI_ATTR VkResult VKAPI_CALL ResetFences(
		VkDevice                                    device,
		uint32_t                                    fenceCount,
		const VkFence*                              pFences) {
	auto& dev = getDevice(device);

	// Per spec, we can assume all associated payload to be finished.
	// It's important we do this *before* resetting the fence since otherwise
	// we might reset it while other threads are querying/waiting upon it/, which
	// might be invalid per vulkan speec.
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

	return dev.dispatch.ResetFences(device, fenceCount, pFences);
}

VKAPI_ATTR VkResult VKAPI_CALL GetFenceStatus(
		VkDevice                                    device,
		VkFence                                     fence) {
	auto& dev = getDevice(device);
	auto& fenceD = dev.fences.get(fence);

	VkResult res = dev.dispatch.GetFenceStatus(device, fence);

	if(res != VK_SUCCESS) {
		return res;
	}

	// When result is VK_SUCCESS, the payload must be completed
	std::lock_guard lock(dev.mutex);
	if(fenceD.submission) {
		auto finished = checkLocked(*fenceD.submission);
		dlg_assert(finished);
	}

	return res;
}

VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(
		VkDevice                                    device,
		uint32_t                                    fenceCount,
		const VkFence*                              pFences,
		VkBool32                                    waitAll,
		uint64_t                                    timeout) {
	auto& dev = getDevice(device);
	VkResult res = dev.dispatch.WaitForFences(device, fenceCount, pFences, waitAll, timeout);

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

// semaphore
VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(
		VkDevice                                    device,
		const VkSemaphoreCreateInfo*                pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkSemaphore*                                pSemaphore) {
	auto& dev = getDevice(device);
	auto res = dev.dispatch.CreateSemaphore(device, pCreateInfo, pAllocator, pSemaphore);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& semaphore = dev.semaphores.add(*pSemaphore);
	semaphore.objectType = VK_OBJECT_TYPE_SEMAPHORE;
	semaphore.dev = &dev;
	semaphore.handle = *pSemaphore;
	semaphore.type = VK_SEMAPHORE_TYPE_BINARY;

	auto* typeCI = findChainInfo<VkSemaphoreTypeCreateInfo,
		VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO>(*pCreateInfo);
	if(typeCI) {
		semaphore.value = typeCI->initialValue;
		semaphore.type = typeCI->semaphoreType;
	}

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroySemaphore(
		VkDevice                                    device,
		VkSemaphore                                 semaphore,
		const VkAllocationCallbacks*                pAllocator) {
	if(!semaphore) {
		return;
	}

	auto& dev = getDevice(device);
	dev.semaphores.mustErase(semaphore);
	dev.dispatch.DestroySemaphore(device, semaphore, pAllocator);
}

// event
VKAPI_ATTR VkResult VKAPI_CALL CreateEvent(
		VkDevice                                    device,
		const VkEventCreateInfo*                    pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkEvent*                                    pEvent) {
	auto& dev = getDevice(device);
	auto res = dev.dispatch.CreateEvent(device, pCreateInfo, pAllocator, pEvent);
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
	if(!event) {
		return;
	}

	auto& dev = getDevice(device);
	dev.events.mustErase(event);
	dev.dispatch.DestroyEvent(device, event, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL GetEventStatus(
		VkDevice                                    device,
		VkEvent                                     event) {
	auto& dev = getDevice(device);
	return dev.dispatch.GetEventStatus(device, event);
}

VKAPI_ATTR VkResult VKAPI_CALL SetEvent(
		VkDevice                                    device,
		VkEvent                                     event) {
	auto& dev = getDevice(device);
	return dev.dispatch.SetEvent(device, event);
}

VKAPI_ATTR VkResult VKAPI_CALL ResetEvent(
		VkDevice                                    device,
		VkEvent                                     event) {
	auto& dev = getDevice(device);
	return dev.dispatch.ResetEvent(device, event);
}

} // namespace vil
