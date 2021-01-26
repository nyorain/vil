#pragma once

#include <device.hpp>
#include <vk/vulkan.h>

struct VkWaylandSurfaceCreateInfoKHR;

namespace vil {

VKAPI_ATTR VkResult VKAPI_CALL CreateWaylandSurfaceKHR(
	VkInstance                                  instance,
	const VkWaylandSurfaceCreateInfoKHR*        pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkSurfaceKHR*                               pSurface);

} // namespace vil
