#pragma once

#include <fwd.hpp>
#include <vk/vulkan.h>

struct VkXlibSurfaceCreateInfoKHR;
struct VkXcbSurfaceCreateInfoKHR;

namespace vil {

VKAPI_ATTR VkResult VKAPI_CALL CreateXlibSurfaceKHR(
	VkInstance                                  instance,
	const VkXlibSurfaceCreateInfoKHR*           pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkSurfaceKHR*                               pSurface);

VKAPI_ATTR VkResult VKAPI_CALL CreateXcbSurfaceKHR(
	VkInstance                                  instance,
	const VkXcbSurfaceCreateInfoKHR*            pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkSurfaceKHR*                               pSurface);

} // namespace vil

