#include <xlib.hpp>
#include <layer.hpp>
#include <util/util.hpp>
#include <swa/swa.h>
#include <gui/gui.hpp>
#include <overlay.hpp>
#include <platform.hpp>
#include <swa/x11.h>

#include <X11/keysym.h>
#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

namespace fuen {

struct X11Platform : SwaPlatform {
	u32 surfaceWindow {};
	xcb_connection_t* connection;
	xcb_key_symbols_t* symbols;
	xcb_connection_t* origConnection;

	void init(Device& dev, unsigned width, unsigned height) override;
	bool pressed(u32 key) const override;
	bool update(Gui& gui) override;
	void activateWindow(bool doActivate) override;
};

void X11Platform::init(Device& dev, unsigned width, unsigned height) {
	// init display
	dpy = swa_display_x11_create("fuencaliente");
	dlg_assert(dpy);

	this->connection = swa_display_x11_connection(dpy);
	this->symbols = xcb_key_symbols_alloc(this->connection);

	initWindow(dev, (void*)(uintptr_t) this->surfaceWindow, width, height);
}

bool X11Platform::pressed(u32 key) const {
	auto keycode = [&]{
		switch(key) {
			case swa_key_backslash:	return XK_backslash;
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

bool X11Platform::update(Gui& gui) {
	if(status == Status::focused) {
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
		}

		auto rk = xcb_grab_keyboard_reply(connection, gkc, nullptr);
		if(rk->status != XCB_GRAB_STATUS_SUCCESS) {
			dlg_trace("pointer grab failed: {}", (u32) rk->status);
		}
	}

	return SwaPlatform::update(gui);
}

void X11Platform::activateWindow(bool doActivate) {
	if(!doActivate) {
		// end our grab
		xcb_ungrab_pointer(this->connection, XCB_TIME_CURRENT_TIME);
		xcb_ungrab_keyboard(this->connection, XCB_TIME_CURRENT_TIME);
		xcb_flush(this->connection);
	}

	SwaPlatform::activateWindow(doActivate);
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

	auto env = std::getenv("FUEN_HOOK_OVERLAY");
	if(!env || *env == '0') {
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

	auto env = std::getenv("FUEN_HOOK_OVERLAY");
	if(!env || *env == '0') {
		return res;
	}

	auto& platform = createData<X11Platform>(*pSurface);
	platform.surfaceWindow = pCreateInfo->window;
	platform.origConnection = pCreateInfo->connection;

	return res;
}

} // namespac fuen
