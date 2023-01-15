#pragma once

#include <device.hpp>
#include <vk/vulkan.h>
#include <nytl/vec.hpp>

namespace vil {

// Can be associated with a VkSurfaceKHR
struct Platform {
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

	Platform();
	virtual ~Platform() = default;

	virtual void init(Device& dev, unsigned width, unsigned height) = 0;
	virtual void resize(unsigned width, unsigned height) = 0;
	virtual State update(Gui& gui) = 0;
	virtual void onEvent() {};

	// VilKey
	// They are read via environment variables
	int toggleKey_ {0};
	int focusKey_ {0};
};

VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(
	VkInstance                                  instance,
	VkSurfaceKHR                                surface,
	const VkAllocationCallbacks*                pAllocator);

} // namespace vil
