#pragma once

#include <vulkan/vulkan.h>

// TODO: support windows
#include <dlfcn.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

// WIP sketch of a API
// Public API kept in vulkan-like code style

typedef void (*PFN_fuenOverlayShow)(VkSwapchainKHR, bool show);

typedef void (*PFN_fuenOverlayMouseMoveEvent)(VkSwapchainKHR, int x, int y);

// They return whether the event was processed by the overlay
typedef bool (*PFN_fuenOverlayMouseButtonEvent)(VkSwapchainKHR, int x, int y, int button, bool press);
typedef bool (*PFN_fuenOverlayMouseWheelEvent)(VkSwapchainKHR, float x, float y);

// TODO: keycodes?
typedef bool (*PFN_fuenOverlayKeyEvent)(VkSwapchainKHR, uint32_t keycode, bool pressed);
typedef bool (*PFN_fuenOverlayTextEvent)(VkSwapchainKHR, const char* utf8);

typedef struct FuenApi {
	PFN_fuenOverlayShow overlayShow;

	// events
	PFN_fuenOverlayMouseMoveEvent overlayMouseMoveEvent;
	PFN_fuenOverlayMouseButtonEvent overlayMouseButtonEvent;
	PFN_fuenOverlayMouseWheelEvent overlayMouseWheelEvent;
	PFN_fuenOverlayKeyEvent overlayKeyEvent;
	PFN_fuenOverlayTextEvent overlayTextEvent;
} FuenApi;

bool fuenLoadApi(FuenApi* api) {
	void* handle = dlopen("libVkLayer_fuencaliente.so", RTLD_NOLOAD | RTLD_LAZY);
	if(!handle) {
		// In this case, the layer wasn't loaded.
		return false;
	}

	api->overlayShow = (PFN_fuenOverlayShow) dlsym(handle, "fuenOverlayShow");
	if(!api->overlayShow) {
		// hm, weird.
		return false;
	}

	// yeah well just assume they'll load fine if overlayShow loaded.
	api->overlayMouseMoveEvent = (PFN_fuenOverlayMouseMoveEvent) dlsym(handle, "fuenOverlayMouseMoveEvent");
	api->overlayMouseButtonEvent = (PFN_fuenOverlayMouseButtonEvent) dlsym(handle, "fuenOverlayMouseButtonEvent");
	api->overlayMouseWheelEvent = (PFN_fuenOverlayMouseWheelEvent) dlsym(handle, "fuenOverlayMouseWheelEvent");
	api->overlayKeyEvent = (PFN_fuenOverlayKeyEvent) dlsym(handle, "fuenOverlayKeyEvent");
	api->overlayTextEvent = (PFN_fuenOverlayTextEvent) dlsym(handle, "fuenOverlayTextEvent");

	return true;
}

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus
