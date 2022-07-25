#include <xlib.hpp>
#include <layer.hpp>
#include <util/util.hpp>
#include <swa/swa.h>
#include <gui/gui.hpp>
#include <overlay.hpp>
#include <swaPlatform.hpp>
#include <swa/x11.h>

#include <X11/keysym.h>
#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

namespace vil {

struct X11Platform : SwaPlatform {
	u32 surfaceWindow {};
	xcb_connection_t* connection;
	xcb_key_symbols_t* symbols;
	xcb_connection_t* origConnection;
	bool grabbingKeyboard {};
	bool grabbingPointer {};

	~X11Platform();
	void init(Device& dev, unsigned width, unsigned height) override;
	bool pressed(u32 key) const override;
	State update(Gui& gui) override;
	void activateWindow(bool doActivate) override;
	void onEvent() override;
};

X11Platform::~X11Platform() {
	dlg_trace("~X11Platform");
	activateWindow(false);
}

void X11Platform::init(Device& dev, unsigned width, unsigned height) {
	// init display
	dpy = swa_display_x11_create("VIL");
	dlg_assert(dpy);

	this->connection = swa_display_x11_connection(dpy);
	this->symbols = xcb_key_symbols_alloc(this->connection);

	initWindow(dev, (void*)(uintptr_t) this->surfaceWindow, width, height);
}

bool X11Platform::pressed(u32 key) const {
	auto keycode = [&]{
		switch(key) {
			case swa_key_equals: return XK_equal;
			case swa_key_f: return XK_F;
			default: dlg_error("Unexpected key"); return 0;
		}
	}();

	xcb_keycode_t* keyCodes = xcb_key_symbols_get_keycode(symbols, keycode);
	dlg_assert(keyCodes);

	xcb_query_keymap_cookie_t keymapCookie = xcb_query_keymap(connection);
	xcb_query_keymap_reply_t* keys = xcb_query_keymap_reply(connection, keymapCookie, nullptr);

	bool pressed = false;
	if(keys && keyCodes[0] != XCB_NO_SYMBOL) {
		int byteIdx = (keyCodes[0] / 8);
		int bitMask = 1 << (keyCodes[0] % 8);
		pressed = (keys->keys[byteIdx] & bitMask);
	}

	free(keyCodes);
	free(keys);

	return pressed;
}

Platform::State X11Platform::update(Gui& gui) {
	if(status == State::focused) {
		// re-activate force grab
		xcb_ungrab_pointer(this->origConnection, XCB_TIME_CURRENT_TIME);
		xcb_ungrab_keyboard(this->origConnection, XCB_TIME_CURRENT_TIME);
		xcb_flush(this->origConnection);

		auto xwin = (xcb_window_t)(uintptr_t) swa_window_native_handle(window);
		auto gpc = xcb_grab_pointer(this->connection, true, xwin, 0,
			XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, /*xwin*/ XCB_NONE,
			XCB_NONE, XCB_TIME_CURRENT_TIME);
		auto gkc = xcb_grab_keyboard(this->connection, true, xwin, XCB_TIME_CURRENT_TIME,
			XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

		auto rp = xcb_grab_pointer_reply(connection, gpc, nullptr);
		if(rp->status != XCB_GRAB_STATUS_SUCCESS) {
			dlg_trace("pointer grab failed: {}", (u32) rp->status);
			grabbingPointer = false;
		} else {
			grabbingPointer = true;
		}

		auto rk = xcb_grab_keyboard_reply(connection, gkc, nullptr);
		if(rk->status != XCB_GRAB_STATUS_SUCCESS) {
			dlg_trace("keyboard grab failed: {}", (u32) rk->status);
			grabbingKeyboard = false;
		} else {
			grabbingKeyboard = true;
		}
	}

	return SwaPlatform::update(gui);
}

void X11Platform::activateWindow(bool doActivate) {
	if(!doActivate) {
		// end our grab
		if(grabbingPointer) {
			xcb_ungrab_pointer(this->connection, XCB_TIME_CURRENT_TIME);
			grabbingPointer = false;
		}

		if(grabbingKeyboard) {
			xcb_ungrab_keyboard(this->connection, XCB_TIME_CURRENT_TIME);
			grabbingKeyboard = false;
		}

		xcb_flush(this->connection);
	}

	SwaPlatform::activateWindow(doActivate);
}

void X11Platform::onEvent() {
	auto* ev = static_cast<const xcb_generic_event_t*>(swa_display_x11_current_event(this->dpy));
	if(ev && status == State::shown) {
		unsigned type = ev->response_type & ~0x80;
		switch(type) {
			case XCB_BUTTON_PRESS: {
				xcb_button_press_event_t* bev = (xcb_button_press_event_t*) ev;
				bev->event = xcb_window_t(surfaceWindow);
				bev->child = bev->event;
				break;
			} case XCB_BUTTON_RELEASE: {
				xcb_button_release_event_t* bev = (xcb_button_release_event_t*) ev;
				bev->event = xcb_window_t(surfaceWindow);
				bev->child = bev->event;
				break;
			} case XCB_MOTION_NOTIFY: {
				xcb_motion_notify_event_t* motion = (xcb_motion_notify_event_t*) ev;
				motion->event = xcb_window_t(surfaceWindow);
				motion->child = motion->event;
				break;
			} case XCB_KEY_PRESS: {
				xcb_key_press_event_t* kev = (xcb_key_press_event_t*) ev;
				kev->event = xcb_window_t(surfaceWindow);
				kev->child = kev->event;
				break;
			} case XCB_KEY_RELEASE: {
				xcb_key_release_event_t* kev = (xcb_key_release_event_t*) ev;
				kev->event = xcb_window_t(surfaceWindow);
				kev->child = kev->event;
				break;
			} case XCB_ENTER_NOTIFY: {
				xcb_enter_notify_event_t* eev = (xcb_enter_notify_event_t*) ev;
				eev->event = xcb_window_t(surfaceWindow);
				eev->child = eev->event;
				break;
			} case XCB_LEAVE_NOTIFY: {
				xcb_leave_notify_event_t* eev = (xcb_enter_notify_event_t*) ev;
				eev->event = xcb_window_t(surfaceWindow);
				eev->child = eev->event;
				break;
			} default:
				// nothing to forward
				return;
		}

		xcb_send_event(connection, 1, surfaceWindow, 0, (const char*) ev);
		xcb_flush(connection);
	}
}

VKAPI_ATTR VkResult VKAPI_CALL CreateXlibSurfaceKHR(
		VkInstance                                  instance,
		const VkXlibSurfaceCreateInfoKHR*           pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkSurfaceKHR*                               pSurface) {
	auto& ini = getData<Instance>(instance);
	auto res = ini.dispatch.CreateXlibSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
	if(res != VK_SUCCESS) {
		return res;
	}

	if(!checkEnvBinary("VIL_HOOK_OVERLAY", false)) {
		return res;
	}

	auto& platform = createData<X11Platform>(*pSurface);
	platform.surfaceWindow = pCreateInfo->window;
	platform.origConnection = XGetXCBConnection(pCreateInfo->dpy);

	return res;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateXcbSurfaceKHR(
		VkInstance                                  instance,
		const VkXcbSurfaceCreateInfoKHR*            pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkSurfaceKHR*                               pSurface) {
	auto& ini = getData<Instance>(instance);
	auto res = ini.dispatch.CreateXcbSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
	if(res != VK_SUCCESS) {
		return res;
	}

	if(!checkEnvBinary("VIL_HOOK_OVERLAY", false)) {
		return res;
	}

	auto& platform = createData<X11Platform>(*pSurface);
	platform.surfaceWindow = pCreateInfo->window;
	platform.origConnection = pCreateInfo->connection;

	return res;
}

} // namespac vil
