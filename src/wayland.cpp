#include <wayland.hpp>
#include <layer.hpp>
#include <data.hpp>
#include "gui.hpp"

#include "imgui/imgui.h"

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <linux/input-event-codes.h>

namespace vil {

struct WaylandPlatform : Platform {
public:
	WaylandPlatform(wl_display* display, wl_surface* surface);
	~WaylandPlatform();

	void update() override;

	// callbacks
	static void registryGlobal(void*, wl_registry*, u32 name, const char* interface, u32 version);
	static void registryGlobalRemove(void*, wl_registry*, u32 name);

	static void seatCapabilities(void*, wl_seat*, u32 caps);
	static void seatName(void*, wl_seat*, const char* name);

	static void keyboardKeymap(void*, wl_keyboard*, u32, i32, u32);
	static void keyboardEnter(void*, wl_keyboard*, u32, wl_surface*, wl_array*);
	static void keyboardLeave(void*, wl_keyboard*, u32, wl_surface*);
	static void keyboardKey(void*, wl_keyboard*, u32, u32, u32, u32);
	static void keyboardMods(void*, wl_keyboard*, u32, u32, u32, u32, u32);
	static void keyboardRepeatInfo(void*, wl_keyboard*, i32, i32);

	static void pointerEnter(void*, wl_pointer*, u32, wl_surface*, wl_fixed_t, wl_fixed_t);
	static void pointerLeave(void*, wl_pointer*, u32, wl_surface*);
	static void pointerMotion(void*, wl_pointer*, u32, wl_fixed_t, wl_fixed_t);
	static void pointerButton(void*, wl_pointer*, u32, u32, u32, u32);
	static void pointerAxis(void*, wl_pointer*, u32, u32, wl_fixed_t);

public:
	wl_display* display {};
	wl_surface* surface {};

	wl_event_queue* queue {};
	wl_registry* registry {};

	wl_seat* seat {};
	wl_pointer* pointer {};
	wl_keyboard* keyboard {};

	bool mouseOver {};
	bool focus {};
};

namespace {

// pointer
void pointerEnter(void* data, wl_pointer*, u32, wl_surface* surf, wl_fixed_t, wl_fixed_t) {
	auto platform = static_cast<WaylandPlatform*>(data);
	platform->mouseOver = (surf == platform->surface);
}
void pointerLeave(void* data, wl_pointer*, u32, wl_surface*) {
	auto platform = static_cast<WaylandPlatform*>(data);
	platform->mouseOver = false;
}
void pointerMotion(void* data, wl_pointer*, u32, wl_fixed_t x, wl_fixed_t y) {
	auto platform = static_cast<WaylandPlatform*>(data);
	if(platform->mouseOver) {
		auto& io = ImGui::GetIO();
		io.MousePos.x = wl_fixed_to_double(x);
		io.MousePos.y = wl_fixed_to_double(y);
	}
}
void pointerButton(void* data, wl_pointer*, u32, u32, u32 button, u32 state) {
	auto platform = static_cast<WaylandPlatform*>(data);
	auto butid = i32(button) - BTN_LEFT;
	if(platform->mouseOver && butid >= 0 && butid < 5) {
		auto& io = ImGui::GetIO();
		io.MouseDown[butid] = state;
	}
}

void pointerAxis(void* data, wl_pointer*, u32, u32, wl_fixed_t val) {
	auto platform = static_cast<WaylandPlatform*>(data);
	if(platform->mouseOver) {
		auto& io = ImGui::GetIO();
		io.MouseWheel = wl_fixed_to_double(val);
	}
}

const struct wl_pointer_listener pointerListener = {
	pointerEnter,
	pointerLeave,
	pointerMotion,
	pointerButton,
	pointerAxis,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
};

// keyboard
// TODO
void keyboardKeymap(void*, wl_keyboard*, u32, i32, u32) {
}
void keyboardEnter(void*, wl_keyboard*, u32, wl_surface*, wl_array*) {
}
void keyboardLeave(void*, wl_keyboard*, u32, wl_surface*) {
}
void keyboardKey(void*, wl_keyboard*, u32, u32, u32, u32) {
}
void keyboardMods(void*, wl_keyboard*, u32, u32, u32, u32, u32) {
}
void keyboardRepeatInfo(void*, wl_keyboard*, i32, i32) {
}

const struct wl_keyboard_listener keyboardListener = {
	keyboardKeymap,
	keyboardEnter,
	keyboardLeave,
	keyboardKey,
	keyboardMods,
	keyboardRepeatInfo,
};

// seat
void seatCapabilities(void* data, wl_seat* seat, u32 caps) {
	auto platform = static_cast<WaylandPlatform*>(data);
	if(caps & WL_SEAT_CAPABILITY_KEYBOARD && !platform->keyboard) {
		platform->keyboard = wl_seat_get_keyboard(seat);
		wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(platform->keyboard), platform->queue);
		wl_keyboard_add_listener(platform->keyboard, &keyboardListener, platform);
	}
	if(caps & WL_SEAT_CAPABILITY_POINTER && !platform->pointer) {
		platform->pointer = wl_seat_get_pointer(seat);
		wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(platform->pointer), platform->queue);
		wl_pointer_add_listener(platform->pointer, &pointerListener, platform);
	}
}
void seatName(void*, wl_seat*, const char*) {
}

const struct wl_seat_listener seatListener = {
	seatCapabilities,
	seatName,
};


// registry
void registryGlobal(void* data, wl_registry*, u32 name, const char* interface, u32 version) {
	auto platform = static_cast<WaylandPlatform*>(data);

	if(!platform->seat && !std::strcmp(interface, wl_seat_interface.name)) {
		auto v = std::min(4u, version);
		auto* seat = wl_registry_bind(platform->registry, name, &wl_seat_interface, v);
		platform->seat = reinterpret_cast<wl_seat*>(seat);
		wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(platform->seat), platform->queue);
		wl_seat_add_listener(platform->seat, &seatListener, platform);
	}
}
void registryGlobalRemove(void*, wl_registry*, u32) {
	// TODO
}

const struct wl_registry_listener registryListener = {
	registryGlobal,
	registryGlobalRemove,
};

} // anon namespace

// WaylandPlatform
WaylandPlatform::WaylandPlatform(wl_display* display, wl_surface* surface) {
	this->display = display;
	this->surface = surface;

	this->queue = wl_display_create_queue(display);
	this->registry = wl_display_get_registry(display);
	wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(registry), queue);

	wl_registry_add_listener(registry, &registryListener, this);
	// Make sure registry gets all global callbacks.
	// Implcitly flushes the display
	wl_display_roundtrip_queue(display, queue);
	// make sure bound globals received all callbacks
	wl_display_roundtrip_queue(display, queue);
}

WaylandPlatform::~WaylandPlatform() {
}

void WaylandPlatform::update() {
	int res = wl_display_dispatch_queue_pending(display, queue);
	dlg_assert(res >= 0);

	auto& io = ImGui::GetIO();
	if(!mouseOver) {
		io.MousePos.x = -FLT_MAX;
		io.MousePos.y = -FLT_MAX;
	}
}

// entry point
VKAPI_ATTR VkResult VKAPI_CALL CreateWaylandSurfaceKHR(
		VkInstance                                  instance,
		const VkWaylandSurfaceCreateInfoKHR*        pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkSurfaceKHR*                               pSurface) {
	auto& ini = getData<Instance>(instance);
	auto res = ini.dispatch.vkCreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
	if(res != VK_SUCCESS) {
		return res;
	}

	createData<WaylandPlatform>(*pSurface, pCreateInfo->display, pCreateInfo->surface);
	dlg_assert(findData(*pSurface));
	return res;
}

} // namespace vil
