#pragma once

#include <device.hpp>
#include <gui/gui.hpp>
#include <thread>
#include <mutex>
#include <atomic>

struct swa_display;
struct swa_window;

namespace fuen {

// Creates a new window using swa and displays the overlay in it.
struct DisplayWindow {
	swa_window* window {};
	Device* dev {};
	Gui gui;

	Queue* presentQueue {};

	VkSurfaceKHR surface {}; // owned by window
	VkSwapchainKHR swapchain {};
	VkSwapchainCreateInfoKHR swapchainCreateInfo {};

	VkSemaphore acquireSem {};

	bool createWindow(Instance&);
	bool initDevice(Device& dev);
	void resize(unsigned w, unsigned h);

	~DisplayWindow();

private:
	std::thread thread_;
	std::atomic<bool> run_ {true};
	std::vector<RenderBuffer> buffers_;

	// TODO: hack
	enum class State {
		initial,
		createWindow,
		windowCreated,
		initDevice,
		mainLoop,
		shutdown,
	};

	std::atomic<State> state_ {State::initial};
	std::condition_variable cv_;
	std::mutex mutex_;

	void initBuffers();
	void destroyBuffers();
	bool initSwapchain();
	void uiThread(Instance& ini);
};

} // namespace fuen
