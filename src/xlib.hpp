#pragma once

#include <device.hpp>
#include <vulkan/vulkan.h>

struct VkXlibSurfaceCreateInfoKHR;
struct VkXcbSurfaceCreateInfoKHR;

namespace fuen {

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

} // namespace fuen

