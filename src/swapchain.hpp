#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <util/intrusive.hpp>
#include <vk/vulkan.h>
#include <chrono>
#include <optional>
#include <memory>

namespace fuen {

struct RecordBatch {
	Queue* queue;
	std::vector<IntrusivePtr<CommandRecord>> submissions;
};

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

	u32 presentCounter {};
	std::vector<RecordBatch> frameSubmissions; // finished
	std::vector<RecordBatch> nextFrameSubmissions; // being built

	Swapchain() = default;
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
