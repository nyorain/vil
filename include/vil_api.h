// vlid: Vulkan Live Introspection & Debugging Layer.
// Copyright (C) 2020-2021 Jan Kelling
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <stdbool.h>
#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
  // Thanks, microsoft.
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN 1
  #endif // WIN32_LEAN_AND_MEAN
  #ifndef NOMINMAX
    #define NOMINMAX 1
  #endif // NOMINMAX

  #include <windows.h>

  #undef near
  #undef far
  #undef ERROR
  #undef MemoryBarrier
  #undef WIN32_LEAN_AND_MEAN
  #undef NOMINMAX
  #undef CreateEvent
  #undef CreateSemaphore
#else
  #include <dlfcn.h>
  #include <errno.h>
#endif

// Forward delcartion from vulkan.h to not include it here.
typedef struct VkDevice_T* VkDevice;

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

// WIP sketch of a API
// Public API kept in vulkan-like code style.
// The functions must be externally synchronized for a given overaly.
// NOTE: header-only by design, no library needs to be linked besides
// 'dl' on unix and 'kernel32' on windows (usually automatically done).
// This will automatically call symbols from the vlid layer, if it is loaded.

typedef struct VilOverlayT* VilOverlay;

/// Creates an overlay for the swapchain last created for the given device.
/// Return NULL on failure. Note that there might be a data race when you
/// create swapchains from multiple threads. No swapchain must be in creation
/// for the same device in another thread while this is called.
/// The overlay will automatically be moved to a new swapchain if it is created
/// with the swapchain associated to this layer in the oldSwapchain
/// member of the VkSwapchainCreateInfoKHR.
/// By default, it is hidden but you could immediately call vlidOverlayShow
/// after this function to make it visible.
///
/// The name is intentionally horribly long to state as much of the terribleness
/// of this function as possible.
/// Due to the way vulkan layers work, only handles with embedded dispatch
/// table (i.e. device and instance) can be passed directly between application
/// and layer. Therefore the function to create an overlay is designed like
/// this.
/// TODO: allow to destroy overlay again.
typedef VilOverlay (*PFN_vilCreateOverlayForLastCreatedSwapchain)(VkDevice);

typedef void (*PFN_vilOverlayShow)(VilOverlay, bool show);

typedef void (*PFN_vilOverlayMouseMoveEvent)(VilOverlay, int x, int y);

// They return whether the event was processed by the overlay
typedef bool (*PFN_vilOverlayMouseButtonEvent)(VilOverlay, unsigned button, bool press);
typedef bool (*PFN_vilOverlayMouseWheelEvent)(VilOverlay, float x, float y);

// TODO: add keycode (expecting linux.h keycodes) and modifiers enums
typedef bool (*PFN_vilOverlayKeyEvent)(VilOverlay, uint32_t keycode, bool pressed);
typedef bool (*PFN_vilOverlayTextEvent)(VilOverlay, const char* utf8);
typedef void (*PFN_vilOverlayKeyboardModifier)(VilOverlay, uint32_t mod, bool active);

typedef struct VilApi {
	PFN_vilCreateOverlayForLastCreatedSwapchain CreateOverlayForLastCreatedSwapchain;

	PFN_vilOverlayShow OverlayShow;

	PFN_vilOverlayMouseMoveEvent OverlayMouseMoveEvent;
	PFN_vilOverlayMouseButtonEvent OverlayMouseButtonEvent;
	PFN_vilOverlayMouseWheelEvent OverlayMouseWheelEvent;
	PFN_vilOverlayKeyEvent OverlayKeyEvent;
	PFN_vilOverlayTextEvent OverlayTextEvent;
	PFN_vilOverlayKeyboardModifier OverlayKeyboardModifier;
} VilApi;

/// Must be called only *after* a vulkan device was created.
/// The loaded api will remain valid only as long as the vulkan device is valid.
/// Returns 0 on success, an error code otherwise.
static inline int vilLoadApi(VilApi* api) {
	// We don't actually load a library here. If vil was loaded as a
	// layer, the shared library must already be present. Otherwise,
	// we want this to fail anyways.

#if defined(_WIN32) || defined(__CYGWIN__)
	// TODO: name of dll depends on compiler used. For MSC it does not
	// have the lib prefix, for gcc/mingw it does.
	HMODULE handle = GetModuleHandleA("VkLayer_live_introspection.dll");

	// We don't have to call FreeLibrary since GetModuleHandle does not increase ref count
	#define vilCloseLib()
	#define vilLoadSym(procName) *(FARPROC*) (&api->procName) = GetProcAddress(handle, "vil" #procName)
	// #define vilError() GetLastError()
#else
	void* handle = dlopen("libVkLayer_live_introspection.so", RTLD_NOLOAD | RTLD_LAZY);

	// We have to call dlclose since our dlopen increases the reference count.
	#define vilCloseLib() dlclose(handle)
	#define vilLoadSym(procName) *(void**) &(api->procName) = dlsym(handle, "vil" #procName)
	// #define vilError() errno
#endif

	if(!handle) {
		// In this case, the layer wasn't loaded.
		return -1;
	}

	vilLoadSym(CreateOverlayForLastCreatedSwapchain);
	if(!api->CreateOverlayForLastCreatedSwapchain) {
		return -2;
	}

	// yeah well just assume they'll load fine if overlayShow loaded.
	vilLoadSym(OverlayShow);
	vilLoadSym(OverlayMouseMoveEvent);
	vilLoadSym(OverlayMouseButtonEvent);
	vilLoadSym(OverlayMouseWheelEvent);
	vilLoadSym(OverlayKeyEvent);
	vilLoadSym(OverlayTextEvent);
	vilLoadSym(OverlayKeyboardModifier);

	vilCloseLib();

#undef vilCloseLib
#undef vilLoadSym

	return 0;
}

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus
