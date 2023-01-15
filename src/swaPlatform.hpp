#pragma once

#include <swa/key.h>
#include <platform.hpp>

struct swa_display;
struct swa_window;

namespace vil {

// Uses swa to create an input-grabbing, invisible child window on the
// given platform. Just needs platform-specific mechanisms for checking
// on the original window.
struct SwaPlatform : Platform {
	swa_display* dpy {};
	swa_window* window {};

	Vec2f guiWinPos {};
	Vec2f guiWinSize {};

	State status {State::hidden};
	bool togglePressed {}; // for toggle key
	bool focusPressed {}; // for focus key

	// for automatic activation/deactivation
	bool doGuiUnfocus {};

	virtual void activateWindow(bool doActivate);
	void resize(unsigned width, unsigned height) override;
	State update(Gui& gui) override;

	// Derived platforms must first initialize the display (using the
	// specific, matching swa backend), then call this for window
	// initialization.
	void initWindow(Device& dev, void* nativeParent, unsigned width, unsigned height);

	// Must return whether the given key is currently pressed on the
	// original window.
	virtual bool pressed(u32 key) const = 0;
};

} // namespace vil
