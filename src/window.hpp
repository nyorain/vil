#pragma once

#include "device.hpp"
#include "renderer.hpp"
#include "gui.hpp"
#include <thread>

struct swa_display;
struct swa_window;

namespace fuen {

// Creates a new window using swa and displays the overlay in it.
struct DisplayWindow {
	swa_window* window {};
	Device* dev {};
	Renderer renderer;
	Gui gui;

	Queue* presentQueue {};

	VkSurfaceKHR surface {};
	VkSwapchainKHR swapchain {};
	VkSwapchainCreateInfoKHR swapchainCreateInfo {};
	Draw draw {};

	VkSemaphore acquireSem {};
	VkSemaphore renderSem {};

	std::vector<RenderBuffer> buffers;
	std::thread thread;
	bool run {true};

	bool createWindow(Instance&);
	void initDevice(Device& dev);
	void resize(unsigned w, unsigned h);
	void initBuffers();
	void destroyBuffers();
	void mainLoop();

	~DisplayWindow();
};

} // namespace fuen
