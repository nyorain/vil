#include <queryPool.hpp>
#include <wrap.hpp>

namespace vil {

VKAPI_ATTR VkResult VKAPI_CALL CreateQueryPool(
		VkDevice                                    device,
		const VkQueryPoolCreateInfo*                pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkQueryPool*                                pQueryPool) {
	auto& dev = getDevice(device);
	auto res = dev.dispatch.CreateQueryPool(device, pCreateInfo, pAllocator, pQueryPool);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& queryPool = dev.queryPools.add(*pQueryPool);
	queryPool.dev = &dev;
	queryPool.handle = *pQueryPool;
	queryPool.ci = *pCreateInfo;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyQueryPool(
		VkDevice                                    device,
		VkQueryPool                                 queryPool,
		const VkAllocationCallbacks*                pAllocator) {
	if(!queryPool) {
		return;
	}

	auto& dev = *mustMoveUnset(device, queryPool)->dev;
	dev.dispatch.DestroyQueryPool(device, queryPool, pAllocator);
}

} // namespace vil
