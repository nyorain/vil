#include <window.hpp>
#include <layer.hpp>
#include <cb.hpp>
#include <sync.hpp>
#include <image.hpp>
#include <util.hpp>
#include <swa/swa.h>
#include <dlg/dlg.hpp>
#include <imgui/imgui.h>
#include <vulkan/vk_enum_string_helper.h>

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

void cbMouseWheel(swa_window*, float x, float y) {
	ImGui::GetIO().MouseWheel = y;
	ImGui::GetIO().MouseWheelH = x;
}

// DisplayWindow
bool DisplayWindow::createWindow(Instance& ini) {
	dlg_assert(ini.display);

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
	ws.surface_settings.vk.instance = bit_cast<std::uintptr_t>(dev->ini->handle);
	ws.surface_settings.vk.get_instance_proc_addr = bit_cast<swa_proc>(dev->ini->dispatch.GetInstanceProcAddr);
	window = swa_display_create_window(ini.display, &ws);
	if(!window) {
		return false;
	}

	swa_window_set_userdata(window, this);

	surface = bit_cast<VkSurfaceKHR>(swa_window_get_vk_surface(window));

	return true;
}

bool DisplayWindow::initDevice(Device& dev) {
	this->dev = &dev;

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
		dlg_error("failed retrieve surface formats: {}", string_VkResult(res));
		return false;
	}

	auto formats = std::make_unique<VkSurfaceFormatKHR[]>(nFormats);
	res = dev.ini->dispatch.GetPhysicalDeviceSurfaceFormatsKHR(dev.phdev, surface,
		&nFormats, formats.get());
	if(res != VK_SUCCESS) {
		dlg_error("failed retrieve surface formats: {}", string_VkResult(res));
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
		dlg_error("Failed to retrieve surface present modes: {}", string_VkResult(res));
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
	res = dev.ini->dispatch.GetPhysicalDeviceSurfaceCapabilitiesKHR(dev.phdev, surface, &caps);
	if(res != VK_SUCCESS) {
		dlg_error("failed retrieve surface caps: {}", string_VkResult(res));
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
		dlg_error("Failed to create vk swapchain: {}", string_VkResult(res));
		return false;
	}

	{
		VkSemaphoreCreateInfo sci;
		sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		VK_CHECK(dev.dispatch.CreateSemaphore(dev.handle, &sci, nullptr, &acquireSem));
		VK_CHECK(dev.dispatch.CreateSemaphore(dev.handle, &sci, nullptr, &renderSem));
	}

	this->draw.init(dev);
	this->gui.init(dev, sci.imageFormat, true);
	initBuffers();

	this->thread = std::thread([&]{ mainLoop(); });

	return true;
}

void DisplayWindow::resize(unsigned w, unsigned h) {
	auto& dev = *this->dev;

	destroyBuffers();

	// recreate swapchain
	VkSurfaceCapabilitiesKHR caps;
	VkResult res = dev.ini->dispatch.GetPhysicalDeviceSurfaceCapabilitiesKHR(dev.phdev, surface, &caps);
	if(res != VK_SUCCESS) {
		dlg_error("failed retrieve surface caps: {}", string_VkResult(res));
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
	res = dev.dispatch.CreateSwapchainKHR(dev.handle, &sci, nullptr, &swapchain);

	dev.dispatch.DestroySwapchainKHR(dev.handle, sci.oldSwapchain, nullptr);
	sci.oldSwapchain = VK_NULL_HANDLE;

	if(res != VK_SUCCESS) {
		dlg_error("Failed to create vk swapchain: {}", string_VkResult(res));
		run = false;
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

	buffers.resize(imgCount);
	for(auto i = 0u; i < imgCount; ++i) {
		buffers[i].init(dev, imgs[i], swapchainCreateInfo.imageFormat,
			swapchainCreateInfo.imageExtent, gui.rp());
	}
}

void DisplayWindow::destroyBuffers() {
	buffers.clear();
}

void DisplayWindow::mainLoop() {
	auto& dev = *this->dev;

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

	while(run && swa_display_dispatch(dev.ini->display, false)) {
		u32 id;
		// render a frame
		VkResult res = dev.dispatch.AcquireNextImageKHR(dev.handle, swapchain,
			UINT64_MAX, acquireSem, VK_NULL_HANDLE, &id);
		if(res == VK_SUBOPTIMAL_KHR) {
			dlg_info("Got suboptimal swapchain (acquire)");
			continue;
		} else if(res == VK_ERROR_OUT_OF_DATE_KHR) {
			dlg_warn("Got out of date swapchain (acquire)");
			continue;
		} else if(res != VK_SUCCESS) {
			dlg_error("vkAcquireNextImageKHR: {}", string_VkResult(res));
			run = false;
			break;
		}

		auto& sci = swapchainCreateInfo;
		VkCommandBufferBeginInfo cbBegin;
		cbBegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		VK_CHECK(dev.dispatch.BeginCommandBuffer(draw.cb, &cbBegin));

		// update modifiers
		io.KeyAlt = false;
		io.KeyShift = false;
		io.KeyCtrl = false;
		io.KeySuper = false;

		auto mods = swa_display_active_keyboard_mods(dev.ini->display);
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

		// TODO
		gui.draw(draw, true);
		auto& drawData = *ImGui::GetDrawData();
		renderer.uploadDraw(draw, drawData);


		// pretty long, terrible critical section...
		{
			// Lock order important. See vkQueueSubmit
			// TODO: queue lock should not be needed here..
			std::lock_guard queueLock(dev.queueMutex);

			// Need this here to prevent resources we use
			// (e.g. the image) from being destroyed. Also to make sure
			// no new submissions are added
			std::lock_guard lock(dev.mutex);

			// TODO: move to gui/renderer

			// Make sure relevant command buffers have completed (and check
			// for latest image layout).
			// We do this while holding the queue lock to make sure
			// no new submissions are done until we are done.
			// TODO: terrible! optimize! Might be possible with semaphores,
			// even tho hard (semaphore pool is harder to manage than
			// fence pool since we can't reset)
			VkImageLayout finalLayout;
			Image** selImg = std::get_if<Image*>(&gui.selected_.handle);
			if(!gui.selected_.image.view) {
				selImg = {};
			}

			if (selImg) {
				auto& img = **selImg;

				finalLayout = img.pendingLayout;
				std::vector<PendingSubmission*> toComplete;
				for(auto it = dev.pending.begin(); it != dev.pending.end();) {
					auto& pending = *it;

					// remove finished pending submissions.
					// important to do this before accessing them.
					if(checkLocked(*pending)) {
						// don't increase iterator as the current one
						// was erased.
						continue;
					}

					bool wait = false;
					for(auto& sub : pending->submissions) {
						for(auto* cb : sub.cbs) {
							auto it = cb->images.find(img.handle);
							if(it == cb->images.end()) {
								continue;
							}

							wait = true;
						}
					}

					if(wait) {
						toComplete.push_back(pending.get());
					}

					++it;
				}

				if(!toComplete.empty()) {
					std::vector<VkFence> fences;
					std::vector<std::mutex*> mutexes;
					for(auto* pending : toComplete) {
						if(pending->appFence) {
							mutexes.push_back(&pending->appFence->mutex);
							fences.push_back(pending->appFence->handle);
						} else {
							fences.push_back(pending->ourFence);
						}
					}

					{
						MultiFenceLock lock(std::move(mutexes));
						VK_CHECK(dev.dispatch.vkWaitForFences(dev.handle,
							fences.size(), fences.data(), true, UINT64_MAX));
					}

					for(auto* pending : toComplete) {
						auto res = checkLocked(*pending);
						// we waited for it above. It should really
						// be completed now.
						dlg_assert(res);
					}
				}

				// Make sure our image is in the right layout.
				// And we are allowed to read it
				// TODO: transfer queue
				VkImageMemoryBarrier imgb = vk::ImageMemoryBarrier();
				imgb.image = img.handle;
				imgb.subresourceRange.aspectMask = renderer.selected.image.aspectMask;
				imgb.subresourceRange.layerCount = 1u;
				imgb.subresourceRange.levelCount = 1u;
				imgb.oldLayout = finalLayout;
				imgb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				imgb.srcAccessMask = {}; // TODO: dunno. Track/figure out possible flags
				imgb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

				dev.dispatch.vkCmdPipelineBarrier(draw.cb,
					VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // wait for everything
					VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, // our rendering
					0, 0, nullptr, 0, nullptr, 1, &imgb);
			}

			renderer.recordDraw(draw, sci.imageExtent, buffers[id].fb, true);

			if(selImg) {
				// return it to original layout
				VkImageMemoryBarrier imgb = vk::ImageMemoryBarrier();
				imgb.image = (*selImg)->handle;
				imgb.subresourceRange.aspectMask = renderer.selected.image.aspectMask;
				imgb.subresourceRange.layerCount = 1u;
				imgb.subresourceRange.levelCount = 1u;
				imgb.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				imgb.newLayout = finalLayout;
				dlg_assert(
					finalLayout != VK_IMAGE_LAYOUT_PREINITIALIZED &&
					finalLayout != VK_IMAGE_LAYOUT_UNDEFINED);
				imgb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
				imgb.srcAccessMask = {}; // TODO: dunno. Track/figure out possible flags

				dev.dispatch.vkCmdPipelineBarrier(draw.cb,
					VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, // our rendering
					VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, // wait in everything
					0, 0, nullptr, 0, nullptr, 1, &imgb);
			}

			dev.dispatch.vkEndCommandBuffer(draw.cb);

			// submit render commands
			VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

			VkSubmitInfo si = vk::SubmitInfo();
			si.commandBufferCount = 1u;
			si.pCommandBuffers = &draw.cb;
			si.waitSemaphoreCount = 1u;
			si.pWaitSemaphores = &acquireSem;
			si.pWaitDstStageMask = &stage;
			si.signalSemaphoreCount = 1u;
			si.pSignalSemaphores = &renderSem;

			// TODO: break, continue etc: all early-returns here are problematic
			// for sync stuff.
			res = dev.dispatch.vkQueueSubmit(dev.gfxQueue->queue, 1, &si, draw.fence);
			if(res != VK_SUCCESS) {
				dlg_error("vkQueueSubmit: {}", vk::name(vk::Result(res)));
				run = false;
				break;
			}

			// present
			VkPresentInfoKHR pi = vk::PresentInfoKHR();
			pi.swapchainCount = 1;
			pi.pSwapchains = &swapchain;
			pi.pImageIndices = &id;
			pi.waitSemaphoreCount = 1;
			pi.pWaitSemaphores = &renderSem;

			// TODO: use an actual present queue...
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
			VK_CHECK(dev.dispatch.vkWaitForFences(dev.handle, 1, &draw.fence, true, UINT64_MAX));
			VK_CHECK(dev.dispatch.vkResetFences(dev.handle, 1, &draw.fence));
		}
	}
}

} // namespace fuen
