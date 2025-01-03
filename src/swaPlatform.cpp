#include <swaPlatform.hpp>
#include <swa/swa.h>
#include <util/util.hpp>
#include <util/dlg.hpp>
#include <gui/gui.hpp>

// NOTE: we know event calls always happen in the same thread as rendering
// so we don't need to use gui-internal even queue and can access imguiIO
// directly

namespace vil {
namespace {

void cbMouseMove(swa_window* win, const swa_mouse_move_event* ev) {
	auto* platform = static_cast<SwaPlatform*>(swa_window_get_userdata(win));

	if(platform->status == SwaPlatform::State::focused) {
		// dlg_trace("overlay mouse move: {} {}", ev->x, ev->y);
		ImGui::GetIO().AddMousePosEvent(ev->x, ev->y);
	}

	platform->onEvent();
}

void cbMouseButton(swa_window* win, const swa_mouse_button_event* ev) {
	auto* platform = static_cast<SwaPlatform*>(swa_window_get_userdata(win));

	bool forward = platform->status == SwaPlatform::State::shown;
	bool handle = platform->status == SwaPlatform::State::focused;

	auto inside =
		ev->x > platform->guiWinPos.x &&
		ev->y > platform->guiWinPos.y &&
		ev->x < platform->guiWinPos.x + platform->guiWinSize.x &&
		ev->y < platform->guiWinPos.y + platform->guiWinSize.y;
	if(platform->status == SwaPlatform::State::focused && !inside) {
		dlg_trace("state: shown (unfocsed)");
		platform->status = SwaPlatform::State::shown;
		handle = false;
		forward = true;
		platform->doGuiUnfocus = true;
	} else if(platform->status == SwaPlatform::State::shown && inside) {
		dlg_trace("state: focused");
		platform->status = SwaPlatform::State::focused;
		handle = true;
		forward = false;
	}

	if(forward) {
		platform->onEvent();
	}

	if(handle) {
		// dlg_trace("overlay mouse button");
		if(ev->button > 0 && ev->button < 6) {
			ImGui::GetIO().AddMousePosEvent(ev->x, ev->y);
			ImGui::GetIO().AddMouseButtonEvent(unsigned(ev->button) - 1, ev->pressed);
		}
	}

}

void cbMouseCross(swa_window* win, const swa_mouse_cross_event* ev) {
	auto* platform = static_cast<SwaPlatform*>(swa_window_get_userdata(win));
	if(platform->status == SwaPlatform::State::focused) {
		if(ev->entered) {
			ImGui::GetIO().AddMousePosEvent(ev->x, ev->y);
		} else {
			ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
		}
	}

	platform->onEvent();
}

void cbKey(swa_window* win, const swa_key_event* ev) {
	auto* platform = static_cast<SwaPlatform*>(swa_window_get_userdata(win));

	if(platform->status == SwaPlatform::State::focused) {
		// dlg_trace("overlay key: {} {}", ev->keycode, ev->pressed);
		if(ev->keycode < 512) {
			ImGui::GetIO().AddKeyEvent(keyToImGui(ev->keycode), ev->pressed);
		}

		if(ev->utf8 && *ev->utf8) {
			ImGui::GetIO().AddInputCharactersUTF8(ev->utf8);
		}
	}

	platform->onEvent();
}

void cbMouseWheel(swa_window* win, float x, float y) {
	auto* platform = static_cast<SwaPlatform*>(swa_window_get_userdata(win));

	if(platform->status == SwaPlatform::State::focused) {
		ImGui::GetIO().AddMouseWheelEvent(x, y);
	}

	platform->onEvent();
}

} // anon namespace

void SwaPlatform::initWindow(Device& dev, void* nativeParent,
		unsigned width, unsigned height) {
	dlg_assert(dpy);

	// init window
	static swa_window_listener listener;
	listener.mouse_move = cbMouseMove;
	listener.mouse_cross = cbMouseCross;
	listener.mouse_button = cbMouseButton;
	listener.mouse_wheel = cbMouseWheel;
	listener.key = cbKey;
	// TODO: add dummy close listener?

	swa_window_settings ws;
	swa_window_settings_default(&ws);
	ws.title = "VIL: Vulkan Introspection";
	ws.width = width;
	ws.height = height;
	ws.listener = &listener;
	ws.surface = swa_surface_none;
	ws.input_only = true;
	ws.hide = true;
	ws.parent = nativeParent;
	ws.cursor.type = swa_cursor_left_pointer;
	window = swa_display_create_window(dpy, &ws);
	dlg_assert(window);

	swa_window_set_userdata(window, this);

	(void) dev;
}

void SwaPlatform::resize(unsigned width, unsigned height) {
	swa_window_set_size(window, width, height);
}

bool updateEdge(bool& val, bool pressed) {
	if(val && !pressed) {
		val = false;
		return true;
	} else if(!val && pressed) {
		val = true;
	}

	return false;
}

Platform::State SwaPlatform::update(Gui& gui) {
	gui.makeImGuiCurrent();

	guiWinPos = gui.windowPos();
	guiWinSize = gui.windowSize();

	if(status != State::focused) {
		if(updateEdge(togglePressed, this->pressed(toggleKey_))) {
			activateWindow(true);
			status = State::focused;
		}
	}

	swa_display_dispatch(this->dpy, false);
	if(status == State::focused) {
		bool toggle = swa_display_key_pressed(this->dpy, (swa_key) toggleKey_);
		bool focus = focusKey_ != swa_key_none && swa_display_key_pressed(this->dpy, (swa_key) focusKey_);
		if(updateEdge(togglePressed, toggle)) {
			activateWindow(false);
			status = State::hidden;
		} else if(updateEdge(focusPressed, focus)) {
			activateWindow(false);
			status = State::shown;
		}
	}

	if(doGuiUnfocus) {
		gui.unfocus = true;
		doGuiUnfocus = false;
	}

	return status;
}

void SwaPlatform::activateWindow(bool doActivate) {
	swa_window_show(this->window, doActivate);
}

} // namespace vil
