# Using VIL with compute-only vulkan applications

So in theory, this works perfectly well, without issues. The layer will
create a window to display all state. In practice, there are a couple of things
to consider.

## Instance extensions

It's not possible at the moment to enable instance extensions from within
the layer. So to display anything meaningful in our layer window using 
vulkan we rely on the application to enable the platform-specific instance
extensions (VK_KHR_surface + a platform extension such as VK_KHR_wayland_surface
or VK_KHR_win32_surface). The swapchain extension can be enabled by us
from inside the layer. See https://github.com/KhronosGroup/Vulkan-Loader/issues/51
for a discussion why it does not work at the moment, it might be allowed
in the future.

TODO: we could have a static inline function in our `api.h` that just
returns which extensions should be enabled depending on the platform.
Querying which extensions to enable is really annoying for compute-only
applications, just to use the layer.

## Shutdown

When you have a compute application that isn't interactive it might just
do its computation thing and shut down. When the application shuts down
(more precisely, when it destroys the vulkan device) the layers window is
closed as well. And the meaningful device state (such as buffers, images,
command buffers etc) are destroyed even before that. So you want to
have at least one (blocking) point of interactivity in your compute-only
application (like a `std::getchar()` before destroying any resources)
to make sure you have all the time you need for debugging and inspection.
Nothing we can do inside the layer to block teardown, really (since we don't
know when it begins).
