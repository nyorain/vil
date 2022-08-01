#pragma once

#include <handle.hpp>

namespace vil {

struct QueryPool : SharedDeviceHandle {
	VkQueryPool handle {};
	VkQueryPoolCreateInfo ci {};

	~QueryPool();
};

VKAPI_ATTR VkResult VKAPI_CALL CreateQueryPool(
    VkDevice                                    device,
    const VkQueryPoolCreateInfo*                pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkQueryPool*                                pQueryPool);

VKAPI_ATTR void VKAPI_CALL DestroyQueryPool(
    VkDevice                                    device,
    VkQueryPool                                 queryPool,
    const VkAllocationCallbacks*                pAllocator);

// No need to hook:
// - GetQueryPoolResults

} // namespace vil
