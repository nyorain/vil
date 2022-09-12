#pragma once

#include <fwd.hpp>
#include <gui/gui.hpp>
#include <gui/render.hpp>
#include <thread>
#include <mutex>
#include <atomic>

struct swa_display;
struct swa_window;

namespace vil {

// Creates a completely new system window using swa and displays the overlay in it.
// Since winapi has some major thread-quirks, we need to create and initialize
// everything inside our ui thread, requiring some synchronous
// thread-communication during intialization.
struct DisplayWindow {
	swa_display* dpy {};
	swa_window* window {};
	Instance* ini {};
	Device* dev {};
	Gui* gui {};

	Queue* presentQueue {};

	VkSurfaceKHR surface {}; // owned by window
	VkSwapchainKHR swapchain {};
	VkSwapchainCreateInfoKHR swapchainCreateInfo {};

	VkSemaphore acquireSem {};

	bool createDisplay();
	bool createWindow(Instance&);
	bool initDevice(Device& dev);
	void resize(unsigned w, unsigned h);

	~DisplayWindow();

private:
	std::thread thread_;
	std::atomic<bool> run_ {true};

	VkImage depthImage_ {};
	VkImageView depthView_ {};
	VkDeviceMemory depthMemory_ {};

	std::vector<RenderBuffer> buffers_;

	enum class State {
		initial,
		createDisplay,
		displayCreated,
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
	void uiThread();
};

} // namespace vil
