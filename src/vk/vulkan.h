#pragma once

#ifdef FUEN_WITH_WIN32
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #define VK_USE_PLATFORM_WIN32_KHR
#endif // FUEN_WITH_WIN32

#ifdef FUEN_WITH_WAYLAND
    #define VK_USE_PLATFORM_WAYLAND_KHR
#endif // FUEN_WITH_WAYLAND

#ifdef FUEN_WITH_X11
    #define VK_USE_PLATFORM_XLIB_KHR
    #define VK_USE_PLATFORM_XCB_KHR
#endif // FUEN_WITH_X11

#include <vulkan/vulkan.h>

#ifdef FUEN_WITH_WIN32
    // thanks bill
    #undef near
    #undef far
    #undef ERROR
    #undef MemoryBarrier
    #undef CreateEvent
    #undef CreateSemaphore
    // #undef WIN32_LEAN_AND_MEAN
    // #undef NOMINMAX
#endif // FUEN_WITH_WIN32