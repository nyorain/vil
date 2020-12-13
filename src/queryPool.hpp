#pragma once

#include <device.hpp>
#include <handle.hpp>

namespace fuen {

struct QueryPool : DeviceHandle {
	VkQueryPool handle {};
	VkQueryPoolCreateInfo ci {};
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

} // namespace fuen
