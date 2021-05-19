#include <fwd.hpp>
#include <vk/vulkan.h>
#include <vil_api.h>
#include <data.hpp>
#include <device.hpp>
#include <window.hpp>
#include <gui/gui.hpp>
#include <swapchain.hpp>
#include <overlay.hpp>
#include <imgui/imgui.h>
#include <swa/swa.h>

#if defined(_WIN32) || defined(__CYGWIN__)
  #define VIL_API __declspec(dllexport)
#elif __GNUC__ >= 4
  #define VIL_API __attribute__((visibility ("default")))
#else
  #define VIL_API // just pray it works.
#endif

using namespace vil;

// TODO: we probably have to lock the mutex for every input call

extern "C" VIL_API VilOverlay vilCreateOverlayForLastCreatedSwapchain(VkDevice vkDevice) {
	auto& dev = getDeviceByLoader(vkDevice);

	VilOverlay ret {};
	{
		std::lock_guard lock(dev.mutex);
		if(!dev.lastCreatedSwapchain) {
			dlg_warn("No last created swapchain (was the last created swapchain destroyed?)");
			return {};
		}

		auto& sc = *dev.lastCreatedSwapchain;
		if(sc.overlay) {
			dlg_warn("Swapchain already had an overlay");
			return {};
		}

		if(dev.gui) {
			dlg_warn("There already is a vil gui; Can't have two of them");
			return {};
		}

		sc.overlay = std::make_unique<vil::Overlay>();
		sc.overlay->init(sc);
		ret = reinterpret_cast<VilOverlay>(sc.overlay.get());
	}

	// When the application creates an overlay, we can close the window
	if(dev.window) {
		dev.window.reset();
	}

	return ret;
}

extern "C" VIL_API void vilOverlayShow(VilOverlay overlay, bool show) {
	auto& ov = *reinterpret_cast<vil::Overlay*>(overlay);
	ov.gui.visible = show;
}

extern "C" VIL_API void vilOverlayMouseMoveEvent(VilOverlay overlay, int x, int y) {
	auto& ov = *reinterpret_cast<vil::Overlay*>(overlay);
	if(ov.gui.visible) {
		ov.gui.imguiIO().MousePos = {float(x), float(y)};
	}
}

// They return whether the event was processed by the overlay
extern "C" VIL_API bool vilOverlayMouseButtonEvent(VilOverlay overlay, unsigned button, bool press) {
	auto& ov = *reinterpret_cast<vil::Overlay*>(overlay);
	if(ov.gui.visible && button < 5) {
		auto& io = ov.gui.imguiIO();
		io.MouseDown[button] = press;
		return io.WantCaptureMouse;
	}

	return false;
}
extern "C" VIL_API bool vilOverlayMouseWheelEvent(VilOverlay overlay, float x, float y) {
	auto& ov = *reinterpret_cast<vil::Overlay*>(overlay);
	if(ov.gui.visible) {
		auto& io = ov.gui.imguiIO();
		io.MouseWheel += y;
		io.MouseWheelH += x;
		return io.WantCaptureMouse;
	}

	return false;
}

extern "C" VIL_API bool vilOverlayKeyEvent(VilOverlay overlay, uint32_t keycode, bool pressed) {
	auto& ov = *reinterpret_cast<vil::Overlay*>(overlay);

	// TODO; remove hardcoded toggle.
	if(keycode == swa_key_backslash && pressed) {
		ov.gui.visible ^= true;
		return true;
	}

	if(!ov.gui.visible) {
		return false;
	}

	auto& io = ov.gui.imguiIO();

	io.KeysDown[keycode] = pressed;
	return io.WantCaptureKeyboard;
}

extern "C" VIL_API bool vilOverlayTextEvent(VilOverlay overlay, const char* utf8) {
	auto& ov = *reinterpret_cast<vil::Overlay*>(overlay);
	if(!ov.gui.visible || !utf8) {
		return false;
	}

	auto& io = ov.gui.imguiIO();
	io.AddInputCharactersUTF8(utf8);
	return io.WantCaptureKeyboard || io.WantTextInput;
}

extern "C" VIL_API void vilOverlayKeyboardModifier(VilOverlay overlay, uint32_t mod, bool active) {
	auto& ov = *reinterpret_cast<vil::Overlay*>(overlay);
	if(!ov.gui.visible) {
		return;
	}

	auto& io = ov.gui.imguiIO();
	switch(mod) {
		case swa_keyboard_mod_alt:
			io.KeyAlt = active;
			break;
		case swa_keyboard_mod_ctrl:
			io.KeyCtrl = active;
			break;
		case swa_keyboard_mod_super:
			io.KeySuper = active;
			break;
		case swa_keyboard_mod_shift:
			io.KeyShift = active;
			break;
		default:
			break;
	}
}
