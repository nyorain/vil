#include "fwd.hpp"
#include "api.h"
#include "data.hpp"
#include "device.hpp"
#include "gui.hpp"
#include "swapchain.hpp"
#include "imgui.h"
#include <swa/swa.h>

#if defined(_WIN32) || defined(__CYGWIN__)
  #define FUEN_API __declspec(dllexport)
#elif __GNUC__ >= 4
  #define FUEN_API __attribute__((visibility ("default")))
#endif

using namespace fuen;

extern "C" FUEN_API void fuenOverlayShow(VkDevice vkDevice, VkSwapchainKHR vkSwapchain, bool show) {
	auto& dev = getData<Device>(vkDevice);
	auto& swapchain = dev.swapchains.get(vkSwapchain);
	if(swapchain.overlay) {
		swapchain.overlay->show = show;
	}
}

extern "C" FUEN_API void fuenOverlayMouseMoveEvent(VkDevice vkDevice, VkSwapchainKHR vkSwapchain, int x, int y) {
	auto& dev = getData<Device>(vkDevice);
	auto& swapchain = dev.swapchains.get(vkSwapchain);
	if(swapchain.overlay && swapchain.overlay->show) {
		ImGui::SetCurrentContext(swapchain.overlay->renderer.imgui);
		auto& io = ImGui::GetIO();
		io.MousePos = {float(x), float(y)};
	}
}

// They return whether the event was processed by the overlay
extern "C" FUEN_API bool fuenOverlayMouseButtonEvent(VkDevice vkDevice, VkSwapchainKHR vkSwapchain, unsigned button, bool press) {
	auto& dev = getData<Device>(vkDevice);
	auto& swapchain = dev.swapchains.get(vkSwapchain);
	if(!swapchain.overlay || !swapchain.overlay->show) {
		return false;
	}

	ImGui::SetCurrentContext(swapchain.overlay->renderer.imgui);
	auto& io = ImGui::GetIO();

	if(button < 5) {
		io.MouseDown[button] = press;
		return io.WantCaptureMouse;
	}

	return false;
}
extern "C" FUEN_API bool fuenOverlayMouseWheelEvent(VkDevice vkDevice, VkSwapchainKHR vkSwapchain, float x, float y) {
	auto& dev = getData<Device>(vkDevice);
	auto& swapchain = dev.swapchains.get(vkSwapchain);
	if(!swapchain.overlay || !swapchain.overlay->show) {
		return false;
	}

	ImGui::SetCurrentContext(swapchain.overlay->renderer.imgui);
	auto& io = ImGui::GetIO();
	io.MouseWheel += y;
	io.MouseWheelH += x;
	return io.WantCaptureMouse;
}

extern "C" FUEN_API bool fuenOverlayKeyEvent(VkDevice vkDevice, VkSwapchainKHR vkSwapchain, uint32_t keycode, bool pressed) {
	auto& dev = getData<Device>(vkDevice);
	auto& swapchain = dev.swapchains.get(vkSwapchain);
	if(!swapchain.overlay) {
		return false;
	}

	// TODO; remove hardcoded toggle.
	if(keycode == swa_key_backslash && pressed) {
		swapchain.overlay->show ^= true;
		return true;
	}

	if(!swapchain.overlay->show) {
		return false;
	}

	ImGui::SetCurrentContext(swapchain.overlay->renderer.imgui);
	auto& io = ImGui::GetIO();

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

extern "C" FUEN_API bool fuenOverlayTextEvent(VkDevice vkDevice, VkSwapchainKHR vkSwapchain, const char* utf8) {
	auto& dev = getData<Device>(vkDevice);
	auto& swapchain = dev.swapchains.get(vkSwapchain);
	if(!swapchain.overlay || !swapchain.overlay->show) {
		return false;
	}

	ImGui::SetCurrentContext(swapchain.overlay->renderer.imgui);
	auto& io = ImGui::GetIO();
	io.AddInputCharactersUTF8(utf8);
	return io.WantCaptureKeyboard;
}

extern "C" FUEN_API void fuenOverlayKeyboardModifier(VkDevice vkDevice, VkSwapchainKHR vkSwapchain, uint32_t mod, bool active) {
	auto& dev = getData<Device>(vkDevice);
	auto& swapchain = dev.swapchains.get(vkSwapchain);
	if(!swapchain.overlay || !swapchain.overlay->show) {
		return;
	}

	ImGui::SetCurrentContext(swapchain.overlay->renderer.imgui);
	auto& io = ImGui::GetIO();
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
