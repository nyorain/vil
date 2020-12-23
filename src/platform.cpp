#include <platform.hpp>
#include <swa/swa.h>

namespace fuen {

void SwaPlatform::init(Device& dev, unsigned width, unsigned height) {
	dlg_assert(dpy);

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

void SwaPlatform::resize(unsigned width, unsigned height) {
	swa_window_set_size(window, width, height);
}

bool SwaPlatform::updateShow() {
}

bool SwaPlatform::update(Gui& gui) {
}

} // namespace fuen
