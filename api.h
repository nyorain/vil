#pragma once

#include <vulkan/vulkan.h>

// TODO: support windows
#include <dlfcn.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

// WIP sketch of a API
// Public API kept in vulkan-like code style.
// The functions must be externally synchronized for the given swapchain.

typedef void (*PFN_fuenOverlayShow)(VkDevice, VkSwapchainKHR, bool show);

typedef void (*PFN_fuenOverlayMouseMoveEvent)(VkDevice, VkSwapchainKHR, int x, int y);

// They return whether the event was processed by the overlay
typedef bool (*PFN_fuenOverlayMouseButtonEvent)(VkDevice, VkSwapchainKHR, unsigned button, bool press);
typedef bool (*PFN_fuenOverlayMouseWheelEvent)(VkDevice, VkSwapchainKHR, float x, float y);

// TODO: keycode and modifiers enum
typedef bool (*PFN_fuenOverlayKeyEvent)(VkDevice, VkSwapchainKHR, uint32_t keycode, bool pressed);
typedef bool (*PFN_fuenOverlayTextEvent)(VkDevice, VkSwapchainKHR, const char* utf8);
typedef void (*PFN_fuenOverlayKeyboardModifier)(VkDevice, VkSwapchainKHR, uint32_t mod, bool active);

typedef struct FuenApi {
	PFN_fuenOverlayShow overlayShow;

	// events
	PFN_fuenOverlayMouseMoveEvent overlayMouseMoveEvent;
	PFN_fuenOverlayMouseButtonEvent overlayMouseButtonEvent;
	PFN_fuenOverlayMouseWheelEvent overlayMouseWheelEvent;
	PFN_fuenOverlayKeyEvent overlayKeyEvent;
	PFN_fuenOverlayTextEvent overlayTextEvent;
	PFN_fuenOverlayKeyboardModifier overlayKeyboardModifier;
} FuenApi;

inline bool fuenLoadApi(FuenApi* api) {
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
	api->overlayKeyboardModifier = (PFN_fuenOverlayKeyboardModifier) dlsym(handle, "fuenOverlayKeyboardModifier");

	return true;
}

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus
