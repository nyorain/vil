#include <fwd.hpp>
#include <vk/vulkan.h>
#include <fuen_api.h>
#include <data.hpp>
#include <device.hpp>
#include <window.hpp>
#include <gui/gui.hpp>
#include <swapchain.hpp>
#include <overlay.hpp>
#include <imgui/imgui.h>
#include <swa/swa.h>

#if defined(_WIN32) || defined(__CYGWIN__)
  #define FUEN_API __declspec(dllexport)
#elif __GNUC__ >= 4
  #define FUEN_API __attribute__((visibility ("default")))
#else
  #define FUEN_API // just pray it works.
#endif

using namespace fuen;

// TODO: we probably have to lock the mutex for every input call

extern "C" FUEN_API FuenOverlay fuenCreateOverlayForLastCreatedSwapchain(VkDevice vkDevice) {
	auto& dev = getDeviceByLoader(vkDevice);

	FuenOverlay ret {};
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

		sc.overlay = std::make_unique<fuen::Overlay>();
		sc.overlay->init(sc);
		ret = reinterpret_cast<FuenOverlay>(sc.overlay.get());
	}

	// TODO: race.
	// When the application creates an overlay, we can close the window
	if(dev.window) {
		dev.window.reset();
	}

	return ret;
}

extern "C" FUEN_API void fuenOverlayShow(FuenOverlay overlay, bool show) {
	auto& ov = *reinterpret_cast<fuen::Overlay*>(overlay);
	ov.show = show;
}

extern "C" FUEN_API void fuenOverlayMouseMoveEvent(FuenOverlay overlay, int x, int y) {
	auto& ov = *reinterpret_cast<fuen::Overlay*>(overlay);
	if(ov.show) {
		ov.gui.imguiIO().MousePos = {float(x), float(y)};
	}
}

// They return whether the event was processed by the overlay
extern "C" FUEN_API bool fuenOverlayMouseButtonEvent(FuenOverlay overlay, unsigned button, bool press) {
	auto& ov = *reinterpret_cast<fuen::Overlay*>(overlay);
	if(ov.show && button < 5) {
		auto& io = ov.gui.imguiIO();
		io.MouseDown[button] = press;
		return io.WantCaptureMouse;
	}

	return false;
}
extern "C" FUEN_API bool fuenOverlayMouseWheelEvent(FuenOverlay overlay, float x, float y) {
	auto& ov = *reinterpret_cast<fuen::Overlay*>(overlay);
	if(ov.show) {
		auto& io = ov.gui.imguiIO();
		io.MouseWheel += y;
		io.MouseWheelH += x;
		return io.WantCaptureMouse;
	}

	return false;
}

extern "C" FUEN_API bool fuenOverlayKeyEvent(FuenOverlay overlay, uint32_t keycode, bool pressed) {
	auto& ov = *reinterpret_cast<fuen::Overlay*>(overlay);

	// TODO; remove hardcoded toggle.
	if(keycode == swa_key_backslash && pressed) {
		ov.show ^= true;
		return true;
	}

	if(!ov.show) {
		return false;
	}

	auto& io = ov.gui.imguiIO();

	// TODO! define mapping in api.h
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


	io.KeysDown[keycode] = pressed;
	return io.WantCaptureKeyboard;
}

extern "C" FUEN_API bool fuenOverlayTextEvent(FuenOverlay overlay, const char* utf8) {
	auto& ov = *reinterpret_cast<fuen::Overlay*>(overlay);
	if(!ov.show || !utf8) {
		return false;
	}

	auto& io = ov.gui.imguiIO();
	io.AddInputCharactersUTF8(utf8);
	return io.WantCaptureKeyboard || io.WantTextInput;
}

extern "C" FUEN_API void fuenOverlayKeyboardModifier(FuenOverlay overlay, uint32_t mod, bool active) {
	auto& ov = *reinterpret_cast<fuen::Overlay*>(overlay);
	if(!ov.show) {
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
