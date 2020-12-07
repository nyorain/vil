#pragma once

#include "device.hpp"
#include "gui.hpp"
#include "renderer.hpp"

namespace fuen {

// Displays the ImGui window directly in the application window, on
// a given swapchain.
struct Overlay {
	Swapchain* swapchain;
	Gui gui;
	Renderer renderer;
	std::vector<RenderBuffer> buffers;
	std::vector<Draw> draws;
	bool show {};

	void init(Swapchain& swapchain);
	void initRenderBuffers(); // called from init
	VkResult drawPresent(Queue& queue, span<const VkSemaphore>, u32 imageIdx);
};

/*
// Can be associated with a VkSurfaceKHR
// TODO: reintroduce this when we try out per-platform child windows
struct Platform {
	virtual ~Platform() = default;
	virtual void update() = 0;
};
*/

} // namespace fuen
