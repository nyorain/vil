#include <sync.hpp>
#include <wrap.hpp>
#include <device.hpp>
#include <data.hpp>
#include <queue.hpp>
#include <wrap.hpp>
#include <threadContext.hpp>
#include <util/util.hpp>

namespace vil {

SyncOp SyncOp::queuePresentDummy {};
SyncOp SyncOp::swapchainAcquireDummy {};
SyncOp SyncOp::doneDummy {};

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

	std::lock_guard lock(dev->mutex);

	// Per spec, we can assume all associated payload to be finished
	// Using while loop since checkLocked will erase from our list.
	while(!signals.empty()) {
		if(!signals.back()->submission) {
			// special signal
			signals.pop_back();
			continue;
		}

		auto finished = checkLocked(*signals.back()->submission->parent);
		dlg_assert(finished);
	}

	while(!waits.empty()) {
		if(!waits.back()->submission) {
			// special signal
			waits.pop_back();
			continue;
		}

		auto finished = checkLocked(*waits.back()->submission->parent);
		dlg_assert(finished);
	}
}

void updateUpperLocked(Semaphore& sem, u64 value) {
	dlg_assert(sem.type == VK_SEMAPHORE_TYPE_TIMELINE);

	assertOwned(sem.dev->mutex);
	if(sem.upperBound >= value) {
		return;
	}

	sem.upperBound = value;
	for(auto& wait : sem.waits) {
		if(wait->value > value) {
			continue;
		}

		if(!wait->submission->active) {
			checkActivateLocked(*wait->submission);
		}
	}
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

	auto& dev = *mustMoveUnset(device, fence)->dev;
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

	auto res = dev.dispatch.ResetFences(device, fenceCount, pFences);
	if(res == VK_ERROR_DEVICE_LOST) {
		onDeviceLost(dev);
	}

	return res;
}

VKAPI_ATTR VkResult VKAPI_CALL GetFenceStatus(
		VkDevice                                    device,
		VkFence                                     fence) {
	auto& dev = getDevice(device);
	auto& fenceD = dev.fences.get(fence);

	VkResult res = dev.dispatch.GetFenceStatus(device, fence);

	if(res != VK_SUCCESS) {
		if(res == VK_ERROR_DEVICE_LOST) {
			onDeviceLost(dev);
		}

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

	if(res == VK_ERROR_DEVICE_LOST) {
		onDeviceLost(dev);
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
	semaphore.dev = &dev;
	semaphore.handle = *pSemaphore;
	semaphore.type = VK_SEMAPHORE_TYPE_BINARY;

	auto* typeCI = findChainInfo<VkSemaphoreTypeCreateInfo,
		VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO>(*pCreateInfo);
	if(typeCI && typeCI->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
		semaphore.lowerBound = typeCI->initialValue;
		semaphore.upperBound = semaphore.lowerBound;
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

	auto& dev = *mustMoveUnset(device, semaphore)->dev;
	dev.dispatch.DestroySemaphore(device, semaphore, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL SignalSemaphore(
		VkDevice                                    device,
		const VkSemaphoreSignalInfo*                pSignalInfo) {
	auto& sem = get(device, pSignalInfo->semaphore);

	{
		std::lock_guard lock(sem.dev->mutex);
		updateUpperLocked(sem, pSignalInfo->value);
		dlg_assert(sem.lowerBound <= pSignalInfo->value);
		sem.lowerBound = pSignalInfo->value;
	}

	auto copy = *pSignalInfo;
	copy.semaphore = sem.handle;

	return sem.dev->dispatch.SignalSemaphore(sem.dev->handle, &copy);
}

VKAPI_ATTR VkResult VKAPI_CALL WaitSemaphores(
		VkDevice                                    device,
		const VkSemaphoreWaitInfo*                  pWaitInfo,
		uint64_t                                    timeout) {
	ThreadMemScope tms;
	auto copy = *pWaitInfo;
	auto sems = tms.alloc<VkSemaphore>(pWaitInfo->semaphoreCount);

	auto& dev = getDevice(device);
	for(auto i = 0u; i < pWaitInfo->semaphoreCount; ++i) {
		sems[i] = get(dev, pWaitInfo->pSemaphores[i]).handle;
	}

	copy.pSemaphores = sems.data();
	auto res = dev.dispatch.WaitSemaphores(dev.handle, &copy, timeout);

	if(res == VK_ERROR_DEVICE_LOST) {
		onDeviceLost(dev);
	}

	return res;

	// TODO: check completed workloads?
}

// event
VKAPI_ATTR VkResult VKAPI_CALL CreateEvent(
		VkDevice                                    device,
		const VkEventCreateInfo*                    pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkEvent*                                    pEvent) {
	auto& dev = getDevice(device);
	auto res = dev.dispatch.CreateEvent(dev.handle, pCreateInfo, pAllocator, pEvent);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto evPtr = IntrusivePtr<Event>(new Event());
	auto& event = *evPtr;
	event.dev = &dev;
	event.handle = *pEvent;

	*pEvent = castDispatch<VkEvent>(event);
	dev.events.mustEmplace(*pEvent, std::move(evPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyEvent(
		VkDevice                                    device,
		VkEvent                                     vkEvent,
		const VkAllocationCallbacks*                pAllocator) {
	if(!vkEvent) {
		return;
	}

	auto& dev = *mustMoveUnset(device, vkEvent)->dev;
	dev.dispatch.DestroyEvent(dev.handle, vkEvent, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL GetEventStatus(
		VkDevice                                    device,
		VkEvent                                     event) {
	auto& ev = get(device, event);
	return ev.dev->dispatch.GetEventStatus(ev.dev->handle, ev.handle);
}

VKAPI_ATTR VkResult VKAPI_CALL SetEvent(
		VkDevice                                    device,
		VkEvent                                     event) {
	auto& ev = get(device, event);
	return ev.dev->dispatch.SetEvent(ev.dev->handle, ev.handle);
}

VKAPI_ATTR VkResult VKAPI_CALL ResetEvent(
		VkDevice                                    device,
		VkEvent                                     event) {
	auto& ev = get(device, event);
	return ev.dev->dispatch.ResetEvent(device, ev.handle);
}

} // namespace vil
