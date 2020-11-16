#pragma once

#include "device.hpp"
#include <vulkan/vulkan.h>

struct VkWaylandSurfaceCreateInfoKHR;

namespace fuen {

VKAPI_ATTR VkResult VKAPI_CALL CreateWaylandSurfaceKHR(
	VkInstance                                  instance,
	const VkWaylandSurfaceCreateInfoKHR*        pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkSurfaceKHR*                               pSurface);

} // namespace fuen
