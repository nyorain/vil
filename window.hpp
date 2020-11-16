#pragma once

#include "device.hpp"
#include "gui.hpp"
#include <thread>

struct swa_display;
struct swa_window;

namespace fuen {

// swa display
struct DisplayWindow {
	swa_display* display;
	swa_window* window;
	Device* dev;
	Renderer renderer;

	VkSurfaceKHR surface {};
	VkSwapchainKHR swapchain {};
	VkSwapchainCreateInfoKHR swapchainCreateInfo {};
	Draw draw {};

	VkSemaphore acquireSem {};
	VkSemaphore renderSem {};

	std::vector<RenderBuffer> buffers;
	std::thread thread;
	bool run {true};

	bool init(Device& dev);
	void resize(unsigned w, unsigned h);
	void initBuffers();
	void destroyBuffers();
	void mainLoop();
};

} // namespace fuen
