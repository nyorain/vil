#pragma once

#include <device.hpp>
#include <overlay.hpp>

// TODO, WIP

struct swa_display;
struct swa_window;

namespace fuen {

// Can be associated with a VkSurfaceKHR
struct Platform {
	virtual ~Platform() = default;

	virtual void init(Device& dev, unsigned width, unsigned height) = 0;
	virtual void resize(unsigned width, unsigned height) = 0;
	virtual bool updateShow() = 0;
	virtual bool update(Gui& gui) = 0;
};

// Uses swa to create an input-grabbing, invisible child window on the
// given platform. Just needs platform-specific mechanisms for checking
// on the original window.
struct SwaPlatform : Platform {
	swa_display* dpy {};
	swa_window* window {};

	bool shown {};
	bool focused {};

	void resize(unsigned width, unsigned height) override;
	bool updateShow() override;
	bool update(Gui& gui) override;

	// Derived platforms must first initialize the display (using the
	// specific, matching swa backend), then call this for window
	// initialization.
	void init(Device& dev, unsigned width, unsigned height) override = 0;

	// Must return whether the given key is currently pressed on the
	// original window.
	virtual bool pressed(u32 key) const = 0;
};

} // namespace fuen
