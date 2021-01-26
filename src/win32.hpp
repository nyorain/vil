#pragma once

#include <fwd.hpp>
#include <vk/vulkan.h>

struct VkWin32SurfaceCreateInfoKHR;

namespace vil {

VKAPI_ATTR VkResult VKAPI_CALL CreateWin32SurfaceKHR(
	VkInstance                                  instance,
	const VkWin32SurfaceCreateInfoKHR*          pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkSurfaceKHR*                               pSurface);

} // namespace vil

