#include "common.hpp"
#include <vkpp/names.hpp>
#include <swa/swa.h>
#include <dlg/dlg.hpp>
#include <imgui/imgui.h>

namespace fuen {

void cbClose(swa_window* win) {
	DisplayWindow* dw = static_cast<DisplayWindow*>(swa_window_get_userdata(win));
	dw->run = false;
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

// DisplayWindow
bool DisplayWindow::init(Device& dev) {
	this->dev = &dev;

	// TODO: might explode. Instead create backend matching the
	// created surface. Otherwise (allowing e.g. full compute debugging
	// as well) just create the display before the instance is created
	// and enable the required extensions.
	display = swa_display_autocreate("fuencaliente");

	static swa_window_listener listener;
	listener.close = cbClose;
	listener.resize = cbResize;
	listener.mouse_move = cbMouseMove;
	listener.mouse_cross = cbMouseCross;
	listener.mouse_button = cbMouseButton;
	listener.key = cbKey;

	swa_window_settings ws;
	swa_window_settings_default(&ws);
	ws.title = "fuencaliente";
	ws.listener = &listener;
	ws.surface = swa_surface_vk;
	ws.surface_settings.vk.instance = reinterpret_cast<std::uintptr_t>(dev.ini->ini);
	ws.surface_settings.vk.get_instance_proc_addr = reinterpret_cast<swa_proc>(dev.dispatch.vkGetInstanceProcAddr);
	window = swa_display_create_window(display, &ws);
	swa_window_set_userdata(window, this);

	surface = reinterpret_cast<VkSurfaceKHR>(swa_window_get_vk_surface(window));

	// init swapchain
	auto& sci = swapchainCreateInfo;
	sci = vk::SwapchainCreateInfoKHR();
	sci.surface = surface;

	VkResult res;

	// check support. mainly for validation layers
	VkBool32 supported;
	res = dev.dispatch.vkGetPhysicalDeviceSurfaceSupportKHR(dev.phdev,
		dev.gfxQueue->family, surface, &supported);
	if(res != VK_SUCCESS || !supported) {
		dlg_error("surface not supported on device");
		return false;
	}

	// query render format
	u32 nFormats;
	res = dev.dispatch.vkGetPhysicalDeviceSurfaceFormatsKHR(dev.phdev, surface, &nFormats, nullptr);
	if(res != VK_SUCCESS || nFormats == 0) {
		dlg_error("failed retrieve surface formats: {}", vk::name(vk::Result(res)));
		return false;
	}

	auto formats = std::make_unique<VkSurfaceFormatKHR[]>(nFormats);
	res = dev.dispatch.vkGetPhysicalDeviceSurfaceFormatsKHR(dev.phdev, surface,
		&nFormats, formats.get());
	if(res != VK_SUCCESS) {
		dlg_error("failed retrieve surface formats: {}", vk::name(vk::Result(res)));
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
	dev.dispatch.vkGetPhysicalDeviceSurfacePresentModesKHR(dev.phdev, surface, &nPresentModes, nullptr);
	auto presentModes = std::make_unique<VkPresentModeKHR[]>(nPresentModes);
	res = dev.dispatch.vkGetPhysicalDeviceSurfacePresentModesKHR(dev.phdev,
		surface, &nPresentModes, presentModes.get());
	if(res != VK_SUCCESS || nPresentModes == 0) {
		dlg_error("Failed to retrieve surface present modes: {}", vk::name(vk::Result(res)));
		return false;
	}

	// this mode is required to be supported
	sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;

	bool vsync = false;
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
	res = dev.dispatch.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.phdev, surface, &caps);
	if(res != VK_SUCCESS) {
		dlg_error("failed retrieve surface caps: {}", vk::name(vk::Result(res)));
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

	res = dev.dispatch.vkCreateSwapchainKHR(dev.dev, &sci, NULL, &swapchain);
	if(res != VK_SUCCESS) {
		dlg_error("Failed to create vk swapchain: {}", vk::name(vk::Result(res)));
		return false;
	}

	{
		VkSemaphoreCreateInfo sci = vk::SemaphoreCreateInfo();
		VK_CHECK(dev.dispatch.vkCreateSemaphore(dev.dev, &sci, nullptr, &acquireSem));
		VK_CHECK(dev.dispatch.vkCreateSemaphore(dev.dev, &sci, nullptr, &renderSem));
	}

	this->draw.init(dev);
	this->renderer.init(dev, sci.imageFormat, true);
	initBuffers();

	this->thread = std::thread([&]{ mainLoop(); });

	return true;
}

void DisplayWindow::resize(unsigned w, unsigned h) {
	auto& dev = *this->dev;

	destroyBuffers();

	// recreate swapchain
	VkSurfaceCapabilitiesKHR caps;
	VkResult res = dev.dispatch.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.phdev, surface, &caps);
	if(res != VK_SUCCESS) {
		dlg_error("failed retrieve surface caps: {}", vk::name(vk::Result(res)));
		run = false;
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
	res = dev.dispatch.vkCreateSwapchainKHR(dev.dev, &sci, nullptr, &swapchain);

	dev.dispatch.vkDestroySwapchainKHR(dev.dev, sci.oldSwapchain, nullptr);
	sci.oldSwapchain = VK_NULL_HANDLE;

	if(res != VK_SUCCESS) {
		dlg_error("Failed to create vk swapchain: {}", vk::name(vk::Result(res)));
		run = false;
		return;
	}

	initBuffers();
}

void DisplayWindow::initBuffers() {
	auto& dev = *this->dev;

	u32 imgCount = 0u;
	VK_CHECK(dev.dispatch.vkGetSwapchainImagesKHR(dev.dev, swapchain, &imgCount, nullptr));
	auto imgs = std::make_unique<VkImage[]>(imgCount);
	VK_CHECK(dev.dispatch.vkGetSwapchainImagesKHR(dev.dev, swapchain, &imgCount, imgs.get()));

	buffers.resize(imgCount);
	for(auto i = 0u; i < imgCount; ++i) {
		buffers[i].init(dev, imgs[i], swapchainCreateInfo.imageFormat,
			swapchainCreateInfo.imageExtent, renderer.rp);
	}
}

void DisplayWindow::destroyBuffers() {
	buffers.clear();
}

void DisplayWindow::mainLoop() {
	auto& dev = *this->dev;

	ImGui::SetCurrentContext(renderer.imgui);
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

	while(run && swa_display_dispatch(display, false)) {
		u32 id;
		// render a frame
		VkResult res = dev.dispatch.vkAcquireNextImageKHR(dev.dev, swapchain,
			UINT64_MAX, acquireSem, VK_NULL_HANDLE, &id);
		if(res == VK_SUBOPTIMAL_KHR) {
			dlg_info("Got suboptimal swapchain (acquire)");
			continue;
		} else if(res == VK_ERROR_OUT_OF_DATE_KHR) {
			dlg_warn("Got out of date swapchain (acquire)");
			continue;
		} else if(res != VK_SUCCESS) {
			dlg_error("vkAcquireNextImageKHR: {}", vk::name(vk::Result(res)));
			run = false;
			break;
		}

		auto& sci = swapchainCreateInfo;
		VkCommandBufferBeginInfo cbBegin = vk::CommandBufferBeginInfo();
		VK_CHECK(dev.dispatch.vkBeginCommandBuffer(draw.cb, &cbBegin));

		// update modifiers
		auto mods = swa_display_active_keyboard_mods(display);
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

		io.DisplaySize.x = sci.imageExtent.width;
		io.DisplaySize.y = sci.imageExtent.height;
		renderer.ensureFontAtlas(draw.cb);

		renderer.drawGui(draw);
		renderer.uploadDraw(draw);
		renderer.recordDraw(draw, sci.imageExtent, buffers[id].fb);

		dev.dispatch.vkEndCommandBuffer(draw.cb);

		// submit render commands
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

		VkSubmitInfo si = vk::SubmitInfo();
		si.pCommandBuffers = &draw.cb;
		si.commandBufferCount = 1u;
		si.waitSemaphoreCount = 1u;
		si.pWaitSemaphores = &acquireSem;
		si.pWaitDstStageMask = &stage;
		si.signalSemaphoreCount = 1u;
		si.pSignalSemaphores = &renderSem;

		res = dev.dispatch.vkQueueSubmit(dev.gfxQueue->queue, 1, &si, draw.fence);
		if(res != VK_SUCCESS) {
			dlg_error("vkQueueSubmit: {}", vk::name(vk::Result(res)));
			run = false;
			break;
		}

		// TODO: use an actual present queue...
		// present
		VkPresentInfoKHR pi = vk::PresentInfoKHR();
		pi.swapchainCount = 1;
		pi.pSwapchains = &swapchain;
		pi.pImageIndices = &id;
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &renderSem;

		res = dev.dispatch.vkQueuePresentKHR(dev.gfxQueue->queue, &pi);
		if(res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
			if(res == VK_ERROR_OUT_OF_DATE_KHR) {
				dlg_warn("Got out of date swapchain (present)");
				continue;
			}

			dlg_error("vkQueuePresentKHR: {}", vk::name(vk::Result(res)));
			run = false;
			break;
		}

		// wait on finish
		VK_CHECK(dev.dispatch.vkWaitForFences(dev.dev, 1, &draw.fence, true, UINT64_MAX));
		VK_CHECK(dev.dispatch.vkResetFences(dev.dev, 1, &draw.fence));
	}
}

} // namespace fuen
