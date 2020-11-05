#include "sync.hpp"
#include "data.hpp"

namespace fuen {

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(
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

VKAPI_ATTR void VKAPI_CALL vkDestroyFence(
		VkDevice                                    device,
		VkFence                                     fence,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	auto* fenceD = dev.fences.find(fence);

	{
		std::lock_guard lock(dev.mutex);
		if(fenceD && fenceD->hooked) {
			fenceD->hooked->hookedFence = {};
		}
	}

	dev.fences.erase(fence);
	dev.dispatch.vkDestroyFence(device, fence, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(
		VkDevice                                    device,
		uint32_t                                    fenceCount,
		const VkFence*                              pFences) {
	auto& dev = getData<Device>(device);

	for(auto i = 0u; i < fenceCount; ++i) {
		auto& fence = dev.fences.get(pFences[i]);
		fence.hookedDone.store(false);
		// don't reset fence.hooked, no effect on that
	}

	return dev.dispatch.vkResetFences(device, fenceCount, pFences);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(
		VkDevice                                    device,
		VkFence                                     fence) {
	auto& dev = getData<Device>(device);
	auto& fenceD = dev.fences.get(fence);

	{
		std::lock_guard lock (dev.mutex);
		if(auto* subm = fenceD.hooked) {
			// even if the fence we forwarded to isn't done, the original
			// fence might still be. So only return the forwarded value
			// here if it is success.
			if(check(*subm)) {
				return VK_SUCCESS;
			}
		} else if(fenceD.hookedDone) {
			return VK_SUCCESS;
		}
	}

	return dev.dispatch.vkGetFenceStatus(device, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(
		VkDevice                                    device,
		uint32_t                                    fenceCount,
		const VkFence*                              pFences,
		VkBool32                                    waitAll,
		uint64_t                                    timeout) {
	auto& dev = getData<Device>(device);

	// have to check here, can't do early return below
	if(!waitAll) {
		for(auto i = 0u; i < fenceCount; ++i) {
			auto& fence = dev.fences.get(pFences[i]);
			if(fence.hookedDone) {
				return VK_SUCCESS;
			}
		}
	}

	bool afterCheck = false;
	std::vector<VkFence> fences;
	fences.reserve(fenceCount);

	{
		std::lock_guard lock(dev.mutex);

		// build fences
		// Important that we don't return from here on, as we potentially increased
		// the waitUpon counter in the loop, have to decrease again after waiting.
		for(auto i = 0u; i < fenceCount; ++i) {
			auto& fence = dev.fences.get(pFences[i]);
			if(fence.hookedDone) {
				continue;
			}

			if(!fence.hooked) {
				fences.push_back(fence.fence);
				continue;
			}

			// could check whether hooked fence is done here already? optimization?
			// if(check(*fence.hooked)) continue;

			afterCheck = true;
			fences.push_back(fence.hooked->fence);
			++fence.hooked->waitedUpon;
		}
	}

	if(fences.empty()) {
		dlg_assert(!afterCheck);
		return VK_SUCCESS;
	}

	auto res = dev.dispatch.vkWaitForFences(device, fenceCount, pFences, waitAll, timeout);

	// we kept at least one sumission alive, check up on it now
	if(afterCheck) {
		std::lock_guard lock(dev.mutex);
		for(auto i = 0u; i < fenceCount; ++i) {
			auto& fence = dev.fences.get(pFences[i]);
			if(!fence.hooked) {
				continue;
			}

			--fence.hooked->waitedUpon;
			check(*fence.hooked);
		}
	}

	return res;
}

} // namespace fuen
