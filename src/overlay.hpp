#pragma once

#include <fwd.hpp>
#include <gui/render.hpp>
#include <gui/gui.hpp>

namespace vil {

// Displays the ImGui window directly in the application window, on
// a given swapchain.
struct Overlay {
	Swapchain* swapchain;
	Gui gui;
	std::vector<RenderBuffer> buffers;

	Platform* platform {};

	void init(Swapchain& swapchain);
	void initRenderBuffers(); // called from init
	VkResult drawPresent(Queue& queue, span<const VkSemaphore>, u32 imageIdx);

	Overlay();
	~Overlay();

	void destroyDepth();

	// Returns whether the two given swapchain create infos are compatible
	// for all overlay-rendering purposes. When this returns true, the swapchain
	// may be changed for an existing overlay later on to another,
	// compatible one (calling initRenderBuffers again afterwards).
	static bool compatible(
		const VkSwapchainCreateInfoKHR& a,
		const VkSwapchainCreateInfoKHR& b);

private:
	VkFormat depthFormat_ {};
	VkImage depthImage_ {};
	VkImageView depthView_ {};
	VkDeviceMemory depthMemory_ {};
};

} // namespace vil
