#pragma once

#ifdef VIL_WITH_WIN32
	#ifndef NOMINMAX
    	#define NOMINMAX
	#endif // NOMINMAX

	#ifndef WIN32_LEAN_AND_MEAN
    	#define WIN32_LEAN_AND_MEAN
	#endif // WIN32_LEAN_AND_MEAN

    #define VK_USE_PLATFORM_WIN32_KHR
#endif // VIL_WITH_WIN32

#ifdef VIL_WITH_WAYLAND
    #define VK_USE_PLATFORM_WAYLAND_KHR
#endif // VIL_WITH_WAYLAND

#ifdef VIL_WITH_X11
    #define VK_USE_PLATFORM_XLIB_KHR
    #define VK_USE_PLATFORM_XCB_KHR
#endif // VIL_WITH_X11

// Basically vulkan.h, only using the platforms we support
#include "vk_platform.h"
#include "vulkan_core.h"

// WAYLAND
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
	#include <wayland-client.h>
	#include "vulkan_wayland.h"
#endif

// WIN32
#ifdef VK_USE_PLATFORM_WIN32_KHR
	#include <windows.h>
	#include "vulkan_win32.h"

	// thanks bill
	#undef near
	#undef far
	#undef ERROR
	#undef MemoryBarrier
	#undef CreateEvent
	#undef CreateSemaphore
	// #undef WIN32_LEAN_AND_MEAN
	// #undef NOMINMAX
#endif

// XCB
#ifdef VK_USE_PLATFORM_XCB_KHR
	#include <xcb/xcb.h>
	#include "vulkan_xcb.h"
#endif

// XLIB
#ifdef VK_USE_PLATFORM_XLIB_KHR
	#include <X11/Xlib.h>

	typedef unsigned long Window;
	typedef struct _XDisplay Display;

	#include "vulkan_xlib.h"

	// ugh, xlib is terrible, too
	#undef None
	#undef ParentRelative
	#undef CopyFromParent
	#undef PointerWindow
	#undef InputFocus
	#undef PointerRoot
	#undef AnyPropertyType
	#undef AnyKey
	#undef AnyButton
	#undef AllTemporary
	#undef CurrentTime
	#undef NoSymbol
	#undef KeyPress
	#undef KeyRelease
	#undef ButtonPress
	#undef ButtonRelease
#endif
