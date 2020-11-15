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
	fence.dev = &dev;
	fence.fence = *pFence;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyFence(
		VkDevice                                    device,
		VkFence                                     fence,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	auto& fenceD = dev.fences.get(fence);

	// per spec, we can assume all associated payload to be finished
	{
		std::lock_guard lock(dev.mutex);
		if(fenceD.submission) {
			auto finished = checkLocked(*fenceD.submission);
			dlg_assert(finished);
		}
	}

	dev.fences.erase(fence);
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

} // namespace fuen
