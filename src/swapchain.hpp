#pragma once

#include <device.hpp>
#include <handle.hpp>
#include <chrono>
#include <optional>

namespace fuen {

struct Swapchain : DeviceHandle {
	VkSwapchainKHR handle {};
	VkSwapchainCreateInfoKHR ci;
	std::unique_ptr<Overlay> overlay;

	// The VkImages associated with this swapchain
	std::vector<Image*> images;

	// TODO: at the moment we simply track times between submissions.
	// We could instead separate between between-present, command buffer
	// execution and in-present timings (to give meaningful timings for
	// applications limited by vsync or cpu).
	using Clock = std::chrono::high_resolution_clock;
	static constexpr auto maxFrameTimings = 1000u;
	std::optional<Clock::time_point> lastPresent {};
	std::vector<Clock::duration> frameTimings;

	~Swapchain();
	void destroy();
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
