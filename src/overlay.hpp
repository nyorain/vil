#pragma once

#include <device.hpp>
#include <gui/gui.hpp>

namespace fuen {

// Displays the ImGui window directly in the application window, on
// a given swapchain.
struct Overlay {
	Swapchain* swapchain;
	Gui gui;
	std::vector<RenderBuffer> buffers;
	bool show {};

	Platform* platform {};

	void init(Swapchain& swapchain);
	void initRenderBuffers(); // called from init
	VkResult drawPresent(Queue& queue, span<const VkSemaphore>, u32 imageIdx);

	// Returns whether the two given swapchain create infos are compatible
	// for all overlay-rendering purposes. When this returns true, the swapchain
	// may be changed for an existing overlay later on to another,
	// compatible one (calling initRenderBuffers again afterwards).
	static bool compatible(
		const VkSwapchainCreateInfoKHR& a,
		const VkSwapchainCreateInfoKHR& b);
};

// Can be associated with a VkSurfaceKHR
struct Platform {
	virtual ~Platform() = default;

	virtual void init(Device& dev, unsigned width, unsigned height) = 0;
	virtual void resize(unsigned width, unsigned height) = 0;
	virtual bool updateShow() = 0;
	virtual bool update(Gui& gui) = 0;
};

} // namespace fuen
