#pragma once

#include "device.hpp"
#include "handle.hpp"

namespace fuen {

struct Swapchain : DeviceHandle {
	VkSwapchainKHR handle {};
	VkSwapchainCreateInfoKHR ci;
	std::unique_ptr<Overlay> overlay;

	std::vector<Image*> images;

	~Swapchain();
};

// api
VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(
	VkDevice                                    device,
	const VkSwapchainCreateInfoKHR*             pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkSwapchainKHR*                             pSwapchain);

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(
	VkDevice                                    device,
	VkSwapchainKHR                             	swapchain,
	const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(
	VkQueue                                     queue,
	const VkPresentInfoKHR*                     pPresentInfo);

} // namespace fuen
