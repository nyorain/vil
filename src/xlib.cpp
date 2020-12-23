#include <xlib.hpp>
#include <layer.hpp>
#include <util.hpp>
#include <swa/swa.h>
#include <gui/gui.hpp>
#include <overlay.hpp>
#include <swa/x11.h>

#include <X11/keysym.h>
#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

namespace fuen {

struct X11Platform : Platform {
	swa_display* dpy {};
	swa_window* window {};
	bool toggleDown {};

	bool doGrab {};
	bool grabDown {};

	// x11 stuff
	u32 surfaceWindow {};
	xcb_connection_t* connection;
	xcb_key_symbols_t* symbols;

	xcb_connection_t* origConnection;

	void init(Device& dev, unsigned width, unsigned height) override;
	void resize(unsigned width, unsigned height) override;
	bool updateShow() override;
	bool update(Gui& gui) override;
};

namespace {

void cbMouseMove(swa_window*, const swa_mouse_move_event* ev) {
	// dlg_trace("overlay mouse move: {} {}", ev->x, ev->y);
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
	// dlg_trace("overlay key: {} {}", ev->keycode, ev->pressed);
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

void X11Platform::init(Device& dev, unsigned width, unsigned height) {
	// init display
	dpy = swa_display_x11_create("fuencaliente");
	dlg_assert(dpy);

	this->connection = swa_display_x11_connection(dpy);
	this->symbols = xcb_key_symbols_alloc(this->connection);

	// init window
	static swa_window_listener listener;
	listener.mouse_move = cbMouseMove;
	listener.mouse_cross = cbMouseCross;
	listener.mouse_button = cbMouseButton;
	listener.mouse_wheel = cbMouseWheel;
	listener.key = cbKey;

	swa_window_settings ws;
	swa_window_settings_default(&ws);
	ws.title = "fuencaliente";
	ws.width = width;
	ws.height = height;
	ws.listener = &listener;
	ws.surface = swa_surface_none;
	ws.input_only = true;
	ws.hide = true;
	ws.parent = (void*)(std::uintptr_t) this->surfaceWindow;
	ws.cursor.type = swa_cursor_left_pointer;
	window = swa_display_create_window(dpy, &ws);
	dlg_assert(window);

	(void) dev;
}

void X11Platform::resize(unsigned width, unsigned height) {
	(void) width;
	(void) height;
	swa_window_set_size(window, width, height);
	xcb_flush(connection);
}

bool X11Platform::updateShow() {
	xcb_keycode_t* keyCodes = xcb_key_symbols_get_keycode(symbols, XK_C);
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

	if(pressed) {
		toggleDown = true;
	} else if(toggleDown) {
		// dlg_trace("showing overlay");

		swa_window_show(window, true);
		xcb_flush(connection);

		doGrab = true;
		toggleDown = false;
		grabDown = false;

		return true;
	}

	return false;
}

bool X11Platform::update(Gui& gui) {
	gui.makeImGuiCurrent();

	if(doGrab) {
		xcb_ungrab_pointer(this->origConnection, XCB_TIME_CURRENT_TIME);
		xcb_ungrab_keyboard(this->origConnection, XCB_TIME_CURRENT_TIME);
		xcb_flush(this->origConnection);

		auto xwin = (xcb_window_t)(uintptr_t) swa_window_native_handle(window);
		auto gpc = xcb_grab_pointer(this->connection, true, xwin, 0,
			XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, xwin,
			XCB_NONE, XCB_TIME_CURRENT_TIME);
		auto gkc = xcb_grab_keyboard(this->connection, true, xwin, XCB_TIME_CURRENT_TIME,
			XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

		auto rp = xcb_grab_pointer_reply(connection, gpc, nullptr);
		// dlg_trace("pointer status: {}", (u32) rp->status);
		auto rk = xcb_grab_keyboard_reply(connection, gkc, nullptr);
		// dlg_trace("keyboard status: {}", (u32) rk->status);
		(void) rp;
		(void) rk;
	}

	// TODO: call multiple times?
	swa_display_dispatch(dpy, false);

	if(ImGui::GetIO().KeysDown[swa_key_f]) {
		grabDown = true;
	} else if(grabDown) {
		grabDown = false;
		doGrab = !doGrab;
		// dlg_info("toggle grab: {}", doGrab);

		if(!doGrab) {
			xcb_ungrab_pointer(this->connection, XCB_TIME_CURRENT_TIME);
			xcb_ungrab_keyboard(this->connection, XCB_TIME_CURRENT_TIME);
			xcb_flush(this->connection);
		}

		swa_window_show(window, doGrab);
		xcb_flush(connection);
	}

	auto ret = true;
	if(ImGui::GetIO().KeysDown[swa_key_c]) {
		toggleDown = true;
	} else if(toggleDown) {
		dlg_trace("hiding overlay");
		swa_window_show(window, false);
		xcb_flush(connection);
		ret = false;
		toggleDown = false;
	}

	return ret;
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

	auto& platform = createData<X11Platform>(*pSurface);
	platform.surfaceWindow = pCreateInfo->window;
	platform.origConnection = pCreateInfo->connection;

	return res;
}

} // namespac fuen
