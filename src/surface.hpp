#pragma once

#include <device.hpp>
#include <vk/vulkan.h>
#include <nytl/vec.hpp>

namespace vil {

// Implemented by the different backends
struct Surface {
	VkSurfaceKHR handle {};

	// Called when a swapchain is created for this surface.
	// Should always implicitly replace the older swapchain.
	// The spec guarantees that no two swapchains for the same surface should
	// exist.
	virtual void swapchainCreated(Swapchain&) {}
	// Called when the current swapchain is destroyed for this surface.
	// NOTE: only called for an explicit DestroySwapchainKHR call, not
	// when a swapchain is passed as oldSwapchain to CreateSwapchainKHR
	virtual void swapchainDestroyed(Swapchain&) {}

	virtual ~Surface() = default;
};

// Surface with over overlay-hook capabilities
struct OverlaySurface : Surface {
	enum class State {
		// overlay and input window are hidden
		hidden,
		// overlay is shown but input window is hidden, i.e. no input
		// can be passed to window
		shown,
		// overlay is shown and input window is active, i.e. input
		// is redirected
		focused,
	};

	OverlaySurface();

	// Called when the swapchain is presented to, should determine
	// whether an overlay should be shown.
	// Called with device mutex locked.
	virtual bool needsRendering(Swapchain& swapchain) = 0;
	// virtual void overlayAdded(Overlay&) {}
	// virtual void overlayRemoved() {}

	// VilKey
	// They are read via environment variables
	int toggleKey_ {0};
	int focusKey_ {0};
};

// Reads the keys (VilKey/swa_key) from VIL_TOGGLE_KEY, VIL_FOCUS_KEY
// environment variables
void readOverlayKeys(int& toggleKey, int& focusKey);

VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(
	VkInstance                                  instance,
	VkSurfaceKHR                                surface,
	const VkAllocationCallbacks*                pAllocator);

} // namespace vil
