#include <fwd.hpp>
#include <vk/vulkan.h>
#include <vil_api.h>
#include <data.hpp>
#include <device.hpp>
#include <window.hpp>
#include <gui/gui.hpp>
#include <swa/swa.h>
#include <util/export.hpp>
#include <swapchain.hpp>
#include <overlay.hpp>
#include <imgui/imgui.h>
#include <swa/swa.h>

using namespace vil;

static_assert(u32(vilKeyA) == u32(swa_key_a));
static_assert(u32(vilKeyX) == u32(swa_key_x));
static_assert(u32(vilKeyC) == u32(swa_key_c));
static_assert(u32(vilKeyV) == u32(swa_key_v));
static_assert(u32(vilKeyZ) == u32(swa_key_z));
static_assert(u32(vilKeyY) == u32(swa_key_y));
static_assert(u32(vilKeyEnter) == u32(swa_key_enter));
static_assert(u32(vilKeyTab) == u32(swa_key_tab));
static_assert(u32(vilKeyLeft) == u32(swa_key_left));
static_assert(u32(vilKeyRight) == u32(swa_key_right));
static_assert(u32(vilKeyDown) == u32(swa_key_down));
static_assert(u32(vilKeyUp) == u32(swa_key_up));
static_assert(u32(vilKeySpace) == u32(swa_key_space));
static_assert(u32(vilKeyBackspace) == u32(swa_key_backspace));
static_assert(u32(vilKeyPageUp) == u32(swa_key_pageup));
static_assert(u32(vilKeyPageDown) == u32(swa_key_pagedown));
static_assert(u32(vilKeyHome) == u32(swa_key_home));
static_assert(u32(vilKeyEnd) == u32(swa_key_end));
static_assert(u32(vilKeyInsert) == u32(swa_key_insert));

extern "C" VIL_EXPORT VilOverlay vilCreateOverlayForLastCreatedSwapchain(VkDevice vkDevice) {
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

extern "C" VIL_EXPORT void vilOverlayShow(VilOverlay overlay, bool show) {
	auto& ov = *reinterpret_cast<vil::Overlay*>(overlay);
	ov.gui.visible = show;
}

extern "C" VIL_EXPORT void vilOverlayMouseMoveEvent(VilOverlay overlay, int x, int y) {
	auto& ov = *reinterpret_cast<vil::Overlay*>(overlay);
	std::lock_guard lock(ov.swapchain->dev->mutex);
	if(ov.gui.visible) {
		ov.gui.imguiIO().MousePos = {float(x), float(y)};
	}
}

// They return whether the event was processed by the overlay
extern "C" VIL_EXPORT bool vilOverlayMouseButtonEvent(VilOverlay overlay, unsigned button, bool press) {
	auto& ov = *reinterpret_cast<vil::Overlay*>(overlay);
	std::lock_guard lock(ov.swapchain->dev->mutex);
	if(ov.gui.visible && button < 5) {
		auto& io = ov.gui.imguiIO();
		io.MouseDown[button] = press;
		return io.WantCaptureMouse;
	}

	return false;
}
extern "C" VIL_EXPORT bool vilOverlayMouseWheelEvent(VilOverlay overlay, float x, float y) {
	auto& ov = *reinterpret_cast<vil::Overlay*>(overlay);
	std::lock_guard lock(ov.swapchain->dev->mutex);
	if(ov.gui.visible) {
		auto& io = ov.gui.imguiIO();
		io.MouseWheel += y;
		io.MouseWheelH += x;
		return io.WantCaptureMouse;
	}

	return false;
}

extern "C" VIL_EXPORT bool vilOverlayKeyEvent(VilOverlay overlay, enum vilKey keycode, bool pressed) {
	auto& ov = *reinterpret_cast<vil::Overlay*>(overlay);
	std::lock_guard lock(ov.swapchain->dev->mutex);

	// TODO; remove hardcoded toggle.
	if(swa_key(keycode) == swa_key_backslash && pressed) {
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

extern "C" VIL_EXPORT bool vilOverlayTextEvent(VilOverlay overlay, const char* utf8) {
	auto& ov = *reinterpret_cast<vil::Overlay*>(overlay);
	std::lock_guard lock(ov.swapchain->dev->mutex);

	if(!ov.gui.visible || !utf8) {
		return false;
	}

	auto& io = ov.gui.imguiIO();
	io.AddInputCharactersUTF8(utf8);
	return io.WantCaptureKeyboard || io.WantTextInput;
}

extern "C" VIL_EXPORT void vilOverlayKeyboardModifier(VilOverlay overlay, enum vilKeyMod mod, bool active) {
	auto& ov = *reinterpret_cast<vil::Overlay*>(overlay);
	std::lock_guard lock(ov.swapchain->dev->mutex);

	if(!ov.gui.visible) {
		return;
	}

	auto& io = ov.gui.imguiIO();
	switch(mod) {
		case vilKeyModAlt:
			io.KeyAlt = active;
			break;
		case vilKeyModCtrl:
			io.KeyCtrl = active;
			break;
		case vilKeyModSuper:
			io.KeySuper = active;
			break;
		case vilKeyModShift:
			io.KeyShift = active;
			break;
		default:
			break;
	}
}
