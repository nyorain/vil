#include <window.hpp>
#include <layer.hpp>
#include <queue.hpp>
#include <cb.hpp>
#include <sync.hpp>
#include <image.hpp>
#include <util.hpp>
#include <swa/swa.h>
#include <dlg/dlg.hpp>
#include <imgui/imgui.h>
#include <vk/enumString.hpp>
#include <chrono>

namespace fuen {
namespace {

void cbClose(swa_window* win) {
	(void) win;
	// TODO: make window closable? not sure if good idea
	// DisplayWindow* dw = static_cast<DisplayWindow*>(swa_window_get_userdata(win));
	// dw->close();
}

void cbResize(swa_window* win, unsigned width, unsigned height) {
	DisplayWindow* dw = static_cast<DisplayWindow*>(swa_window_get_userdata(win));
	dw->resize(width, height);
}

void cbMouseMove(swa_window*, const swa_mouse_move_event* ev) {
	ImGui::GetIO().MousePos.x = ev->x;
	ImGui::GetIO().MousePos.y = ev->y;
}

void cbMouseButton(swa_window*, const swa_mouse_button_event* ev) {
	if(ev->button > 0 && ev->button < 6) {
		ImGui::GetIO().MouseDown[unsigned(ev->button) - 1] = ev->pressed;
	}
}

void cbMouseCross(swa_window*, const swa_mouse_cross_event* ev) {
	if(ev->entered) {
		ImGui::GetIO().MousePos.x = ev->x;
		ImGui::GetIO().MousePos.y = ev->y;
	} else {
		ImGui::GetIO().MousePos.x = -FLT_MAX;
		ImGui::GetIO().MousePos.y = -FLT_MAX;
	}
}

void cbKey(swa_window*, const swa_key_event* ev) {
	if(ev->keycode < 512) {
		ImGui::GetIO().KeysDown[ev->keycode] = ev->pressed;
	}

	if(ev->utf8 && *ev->utf8) {
		ImGui::GetIO().AddInputCharactersUTF8(ev->utf8);
	}
}

void cbMouseWheel(swa_window*, float x, float y) {
	ImGui::GetIO().MouseWheel = y;
	ImGui::GetIO().MouseWheelH = x;
}

} // anon namespace

// DisplayWindow
DisplayWindow::~DisplayWindow() {
	run_.store(false);
	cv_.notify_all();
	if(thread_.joinable()) {
		thread_.join();
	}

	if(swapchain) {
		dev->dispatch.DestroySwapchainKHR(dev->handle, swapchain, nullptr);
	}

	if(window) {
		swa_window_destroy(window);
	}

	if(acquireSem) {
		dev->dispatch.DestroySemaphore(dev->handle, acquireSem, nullptr);
	}
}

bool DisplayWindow::createDisplay() {
	state_.store(State::createDisplay);
	this->thread_ = std::thread([&]{ uiThread(); });

	// wait until window was created
	// It's important we create display & window in the ui thread (mainly windows bullshittery).
	std::unique_lock lock(mutex_);
	while(state_.load() == State::createDisplay) {
		cv_.wait(lock);
	}

	return state_.load() == State::displayCreated && this->dpy;
}

bool DisplayWindow::createWindow(Instance& ini) {
	dlg_assert(dpy);
	this->ini = &ini;

	std::unique_lock lock(mutex_);
	state_.store(State::createWindow);
	cv_.notify_one();
	while(state_.load() == State::createWindow) {
		cv_.wait(lock);
	}

	return state_.load() == State::windowCreated && this->surface;
}

bool DisplayWindow::initDevice(Device& dev) {
	dlg_assert(this->surface);
	this->dev = &dev;

	std::unique_lock lock(mutex_);
	state_.store(State::initDevice);
	cv_.notify_one();

	while(state_.load() == State::initDevice) {
		cv_.wait(lock);
	}

	return state_.load() == State::mainLoop;
}

bool DisplayWindow::initSwapchain() {
	dlg_assert(this->dev);
	auto& dev = *this->dev;

	// init swapchain
	auto& sci = this->swapchainCreateInfo;
	sci = {};
	sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	sci.surface = surface;

	VkResult res;

	// check support. mainly for validation layers
	VkBool32 supported;
	res = dev.ini->dispatch.GetPhysicalDeviceSurfaceSupportKHR(dev.phdev,
		dev.gfxQueue->family, surface, &supported);
	if(res != VK_SUCCESS || !supported) {
		dlg_error("surface not supported on device");
		return false;
	}

	// query render format
	u32 nFormats;
	res = dev.ini->dispatch.GetPhysicalDeviceSurfaceFormatsKHR(dev.phdev, surface, &nFormats, nullptr);
	if(res != VK_SUCCESS || nFormats == 0) {
		dlg_error("failed retrieve surface formats: {}", vk::name(res));
		return false;
	}

	auto formats = std::make_unique<VkSurfaceFormatKHR[]>(nFormats);
	res = dev.ini->dispatch.GetPhysicalDeviceSurfaceFormatsKHR(dev.phdev, surface,
		&nFormats, formats.get());
	if(res != VK_SUCCESS) {
		dlg_error("failed retrieve surface formats: {}", vk::name(res));
		return false;
	}

	// try to find a format matching our needs if we don't have
	// free choice
	sci.imageFormat = formats[0].format;
	sci.imageColorSpace = formats[0].colorSpace;
	if(nFormats == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
		sci.imageFormat = VK_FORMAT_B8G8R8A8_SRGB;
	}

	// Get available present modes
	u32 nPresentModes;
	dev.ini->dispatch.GetPhysicalDeviceSurfacePresentModesKHR(dev.phdev, surface, &nPresentModes, nullptr);
	auto presentModes = std::make_unique<VkPresentModeKHR[]>(nPresentModes);
	res = dev.ini->dispatch.GetPhysicalDeviceSurfacePresentModesKHR(dev.phdev,
		surface, &nPresentModes, presentModes.get());
	if(res != VK_SUCCESS || nPresentModes == 0) {
		dlg_error("Failed to retrieve surface present modes: {}", vk::name(res));
		return false;
	}

	// this mode is required to be supported
	sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;

	bool vsync = true;
	if(!vsync) {
		for (size_t i = 0; i < nPresentModes; i++) {
			if (presentModes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
				sci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
				break;
			} else if (presentModes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
				sci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
			} else if (presentModes[i] == VK_PRESENT_MODE_FIFO_RELAXED_KHR) {
				sci.presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
			}
		}
	}

	VkSurfaceCapabilitiesKHR caps;
	res = dev.ini->dispatch.GetPhysicalDeviceSurfaceCapabilitiesKHR(dev.phdev, surface, &caps);
	if(res != VK_SUCCESS) {
		dlg_error("failed retrieve surface caps: {}", vk::name(res));
		return false;
	}

	uint32_t prefImageCount = caps.minImageCount + 1;
	if((caps.maxImageCount > 0) && (prefImageCount > caps.maxImageCount)) {
		prefImageCount = caps.maxImageCount;
	}

	// transformation
	VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	if(!(caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)) {
		transform = caps.currentTransform;
	}

	// use alpha if possible
	VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	VkCompositeAlphaFlagBitsKHR alpha_flags[] = {
		VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
		VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
		VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
	};

	for(int i = 0; i < 4; ++i) {
		if (caps.supportedCompositeAlpha & alpha_flags[i]) {
			alpha = alpha_flags[i];
			break;
		}
	}

	// In this case, we can freely choose the size.
	if(caps.currentExtent.width == 0xFFFFFFFFu) {
		sci.imageExtent.width = SWA_FALLBACK_WIDTH;
		sci.imageExtent.height = SWA_FALLBACK_HEIGHT;
	} else {
		sci.imageExtent.width = caps.currentExtent.width;
		sci.imageExtent.height = caps.currentExtent.height;
	}

	// usage
	dlg_assert(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	// create swapchain
	sci.minImageCount = prefImageCount;
	sci.preTransform = transform;
	sci.imageArrayLayers = 1;
	sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	sci.clipped = VK_TRUE;
	sci.compositeAlpha = alpha;

	res = dev.dispatch.CreateSwapchainKHR(dev.handle, &sci, NULL, &swapchain);
	if(res != VK_SUCCESS) {
		dlg_error("Failed to create vk swapchain: {}", vk::name(res));
		return false;
	}

	dlg_assertm(swapchain, "We likely called the stub vkCreateSwapchainKHR "
		"implementation, maybe we didn't enable the extension correctly");

	{
		VkSemaphoreCreateInfo sci {};
		sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		VK_CHECK(dev.dispatch.CreateSemaphore(dev.handle, &sci, nullptr, &acquireSem));
	}

	this->gui.init(dev, sci.imageFormat, true);
	initBuffers();

	return true;
}

void DisplayWindow::resize(unsigned w, unsigned h) {
	auto& dev = *this->dev;

	destroyBuffers();

	// recreate swapchain
	VkSurfaceCapabilitiesKHR caps;
	VkResult res = dev.ini->dispatch.GetPhysicalDeviceSurfaceCapabilitiesKHR(dev.phdev, surface, &caps);
	if(res != VK_SUCCESS) {
		dlg_error("failed retrieve surface caps: {}", vk::name(res));
		run_.store(false);
		return;
	}

	auto& sci = swapchainCreateInfo;
	if(caps.currentExtent.width == 0xFFFFFFFFu) {
		sci.imageExtent.width = w;
		sci.imageExtent.height = h;
	} else {
		sci.imageExtent.width = caps.currentExtent.width;
		sci.imageExtent.height = caps.currentExtent.height;
	}

	sci.oldSwapchain = swapchain;
	swapchain = {};

	res = dev.dispatch.CreateSwapchainKHR(dev.handle, &sci, nullptr, &swapchain);

	dev.dispatch.DestroySwapchainKHR(dev.handle, sci.oldSwapchain, nullptr);
	sci.oldSwapchain = VK_NULL_HANDLE;

	if(res != VK_SUCCESS) {
		dlg_error("Failed to create vk swapchain: {}", vk::name(res));
		run_.store(false);
		return;
	}

	initBuffers();
}

void DisplayWindow::initBuffers() {
	auto& dev = *this->dev;

	u32 imgCount = 0u;
	VK_CHECK(dev.dispatch.GetSwapchainImagesKHR(dev.handle, swapchain, &imgCount, nullptr));
	auto imgs = std::make_unique<VkImage[]>(imgCount);
	VK_CHECK(dev.dispatch.GetSwapchainImagesKHR(dev.handle, swapchain, &imgCount, imgs.get()));

	buffers_.resize(imgCount);
	for(auto i = 0u; i < imgCount; ++i) {
		buffers_[i].init(dev, imgs[i], swapchainCreateInfo.imageFormat,
			swapchainCreateInfo.imageExtent, gui.rp());
	}
}

void DisplayWindow::destroyBuffers() {
	gui.finishDraws();
	buffers_.clear();
}

void DisplayWindow::uiThread() {
	// initialization
	{
		std::unique_lock lock(mutex_);

		// step 0: display creation
		dlg_assert(state_.load() == State::createDisplay);
		dpy = swa_display_autocreate("fuencaliente");
		if (!dpy) {
			state_.store(State::shutdown);
			cv_.notify_one();
			return;
		}

		state_.store(State::displayCreated);
		cv_.notify_one();

		// wait for step 1
		while(run_.load() && state_.load() != State::createWindow) {
			cv_.wait(lock);
		}

		if(!run_.load()) {
			return;
		}

		// step 1: window creation
		dlg_assert(this->ini);

		static swa_window_listener listener;
		listener.close = cbClose;
		listener.resize = cbResize;
		listener.mouse_move = cbMouseMove;
		listener.mouse_cross = cbMouseCross;
		listener.mouse_button = cbMouseButton;
		listener.mouse_wheel = cbMouseWheel;
		listener.key = cbKey;

		swa_window_settings ws;
		swa_window_settings_default(&ws);
		ws.title = "fuencaliente";
		ws.listener = &listener;
		ws.surface = swa_surface_vk;
		ws.surface_settings.vk.instance = bit_cast<std::uintptr_t>(this->ini->handle);
		ws.surface_settings.vk.get_instance_proc_addr = bit_cast<swa_proc>(this->ini->dispatch.GetInstanceProcAddr);
		window = swa_display_create_window(dpy, &ws);
		if(!window) {
			state_.store(State::shutdown);
			cv_.notify_one();
			return;
		}

		swa_window_set_userdata(window, this);
		this->surface = bit_cast<VkSurfaceKHR>(swa_window_get_vk_surface(window));

		state_.store(State::windowCreated);
		cv_.notify_one();

		// wait for step 2
		while(run_.load() && state_.load() != State::initDevice) {
			cv_.wait(lock);
		}

		if(!run_.load()) {
			return;
		}

		// step 2: swapchain creation
		if(!initSwapchain()) {
			state_.store(State::shutdown);
			cv_.notify_one();
			return;
		}

		state_.store(State::mainLoop);
		cv_.notify_one();
	}

	auto& dev = *this->dev;

	// run main loop!
	dlg_assert(this->presentQueue);
	dlg_assert(this->swapchain);

	gui.makeImGuiCurrent();
	auto& io = ImGui::GetIO();
	io.KeyMap[ImGuiKey_A] = swa_key_a;
	io.KeyMap[ImGuiKey_C] = swa_key_c;
	io.KeyMap[ImGuiKey_V] = swa_key_v;
	io.KeyMap[ImGuiKey_X] = swa_key_x;
	io.KeyMap[ImGuiKey_Y] = swa_key_y;
	io.KeyMap[ImGuiKey_Z] = swa_key_z;
	io.KeyMap[ImGuiKey_Enter] = swa_key_enter;
	io.KeyMap[ImGuiKey_Delete] = swa_key_del;
	io.KeyMap[ImGuiKey_Space] = swa_key_space;
	io.KeyMap[ImGuiKey_LeftArrow] = swa_key_left;
	io.KeyMap[ImGuiKey_DownArrow] = swa_key_down;
	io.KeyMap[ImGuiKey_RightArrow] = swa_key_right;
	io.KeyMap[ImGuiKey_UpArrow] = swa_key_up;
	io.KeyMap[ImGuiKey_Escape] = swa_key_escape;
	io.KeyMap[ImGuiKey_Tab] = swa_key_tab;
	io.KeyMap[ImGuiKey_Backspace] = swa_key_backspace;

	using Clock = std::chrono::high_resolution_clock;
	auto timeIt = [](auto& now, auto name) {
		auto newNow = Clock::now();
		auto dist = newNow - now;
		dlg_trace("time {}: {}", name,
			std::chrono::duration_cast<std::chrono::milliseconds>(dist).count());
		now = newNow;
	};

	while(run_.load()) {
		auto now = Clock::now();

		if(!swa_display_dispatch(dpy, false)) {
			run_.store(false);
			break;
		}

		if(!run_.load()) {
			break;
		}

		dlg_assert(this->swapchain);

		u32 imageIdx;
		// render a frame
		// TODO: use a fence instead of a semaphore here and just wait
		// for it (could even still dispatch events during that time)?
		// would potentially mean less latency since that waiting for
		// vsync currently effectively happens at the end of Gui::draw
		// (where we wait for the submission).
		// TODO: we also might wanna fix refreshing of this window
		// to a maximum frame rate. We don't really need those dank 144hz
		// but will *significantly* block other vulkan progress (due to
		// gui mutex locking) when rendering at high rates.
		VkResult res = dev.dispatch.AcquireNextImageKHR(dev.handle, swapchain,
			UINT64_MAX, acquireSem, VK_NULL_HANDLE, &imageIdx);
		if(res == VK_SUBOPTIMAL_KHR) {
			dlg_info("Got suboptimal swapchain (acquire)");
			continue;
		} else if(res == VK_ERROR_OUT_OF_DATE_KHR) {
			dlg_warn("Got out of date swapchain (acquire)");
			continue;
		} else if(res != VK_SUCCESS) {
			dlg_error("vkAcquireNextImageKHR: {}", vk::name(res));
			run_.store(false);
			break;
		}

		// update modifiers
		io.KeyAlt = false;
		io.KeyShift = false;
		io.KeyCtrl = false;
		io.KeySuper = false;

		auto mods = swa_display_active_keyboard_mods(dpy);
		if(mods & swa_keyboard_mod_alt) {
			io.KeyAlt = true;
		}
		if(mods & swa_keyboard_mod_ctrl) {
			io.KeyCtrl = true;
		}
		if(mods & swa_keyboard_mod_super) {
			io.KeySuper = true;
		}
		if(mods & swa_keyboard_mod_shift) {
			io.KeyShift = true;
		}

		auto& sci = swapchainCreateInfo;
		io.DisplaySize.x = sci.imageExtent.width;
		io.DisplaySize.y = sci.imageExtent.height;

		// Let gui render frame
		dlg_assert(imageIdx < buffers_.size());

		Gui::FrameInfo frameInfo;
		frameInfo.extent = sci.imageExtent;
		frameInfo.imageIdx = imageIdx;
		frameInfo.fb = buffers_[imageIdx].fb;
		frameInfo.fullscreen = true;
		frameInfo.presentQueue = this->presentQueue->handle;
		frameInfo.swapchain = swapchain;
		auto sems = {acquireSem};
		frameInfo.waitSemaphores = sems;

		auto frameRes = gui.renderFrame(frameInfo);
		(void) frameRes;

		timeIt(now, "frame");

		// NOTE: currently done in gui but may not be like this in future
		// if(frameRes.draw && frameRes.draw->inUse) {
		// 	// wait on finish
		// 	VK_CHECK(dev.dispatch.WaitForFences(dev.handle, 1, &frameRes.draw->fence, true, UINT64_MAX));
		// 	VK_CHECK(dev.dispatch.ResetFences(dev.handle, 1, &frameRes.draw->fence));
		// 	frameRes.draw->inUse = false;
		// 	frameRes.draw->usedHandles.clear();
		// }
	}

	gui.finishDraws(); // NOTE: shouldn't be needed with current blocking rendering
	state_.store(State::shutdown);
	cv_.notify_one();
	dlg_trace("Exiting window thread");
}

} // namespace fuen
