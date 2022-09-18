#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <frame.hpp>
#include <util/intrusive.hpp>
#include <vk/vulkan.h>
#include <chrono>
#include <optional>
#include <array>
#include <memory>

namespace vil {

struct Swapchain : SharedDeviceHandle {
	static constexpr auto objectType = VK_OBJECT_TYPE_SWAPCHAIN_KHR;

	VkSwapchainKHR handle {};
	VkSwapchainCreateInfoKHR ci;
	std::unique_ptr<Overlay> overlay;

	// The VkImages associated with this swapchain
	std::vector<Image*> images;

	// NOTE: at the moment we simply track times between presents.
	// We could instead separate between between-present, command buffer
	// execution and in-present timings (to give meaningful timings for
	// applications limited by vsync or cpu).
	using Clock = std::chrono::high_resolution_clock;
	// NOTE: maybe rather make this dependent on time it takes to display
	// them? e.g. store frames from the last X seconds. And then
	// maybe take median over N frames so we always also get the
	// same total sample count (which is important for UI).
	// e.g. Always show results from the last 10 seconds with 5 bars
	// per second. Could even make those numbrs configurable in UI,
	// add a "pure" mode or something (because this accumulation
	// might show misleading numbers; no real frame timings)
	static constexpr auto maxFrameTimings = 300u;
	std::optional<Clock::time_point> lastPresent {};
	std::vector<Clock::duration> frameTimings;

	// Counter that is increased each time VkQueuePresentKHR on this
	// swapchain is called. Not reset on swapchain recreation.
	u64 presentCounter {};

	static constexpr auto frameSubmissionCount = 8u;
	std::array<FrameSubmissions, frameSubmissionCount> frameSubmissions;
	FrameSubmissions nextFrameSubmissions; // currently being built

	// Whether images from this swapchain support sampling.
	// We will try to set this, if possible for overlay blur.
	bool supportsSampling {};

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

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImageKHR(
    VkDevice                                    device,
    VkSwapchainKHR                              swapchain,
    uint64_t                                    timeout,
    VkSemaphore                                 semaphore,
    VkFence                                     fence,
    uint32_t*                                   pImageIndex);

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImage2KHR(
    VkDevice                                    device,
    const VkAcquireNextImageInfoKHR*            pAcquireInfo,
    uint32_t*                                   pImageIndex);

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(
	VkQueue                                     queue,
	const VkPresentInfoKHR*                     pPresentInfo);

VKAPI_ATTR VkResult VKAPI_CALL GetSwapchainImagesKHR(
    VkDevice                                    device,
    VkSwapchainKHR                              swapchain,
    uint32_t*                                   pSwapchainImageCount,
    VkImage*                                    pSwapchainImages);

} // namespace vil
