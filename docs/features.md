# Feature support overview

## Vulkan 1.0

- Anything with sparse/aliasing memory: not implemented/tested at all, 
  will likely assert/crash in layer
- All other core commands and features should work

## Vulkan 1.1

- Anything with device masks is missing; might crash.
- no support for render pass multiview (might crash at the moment)
- no support for protected submit
- ui: for YCbCr conversion handles not tracked/shown
- ui: no support for variable pointers buffer

## Vulkan 1.2

- vkCmd* commands are implemented
- no support for semaphore types in ui
- no support for descriptor indexing
- no support for FramebufferAttachmentCreateinfo etc

## Non-core extensions

Extensions promoted to core not explicitly mentioned here, see above.

- KHR_swapchain
- KHR_surface (tested overlay platforms: xlib, xcb)
	- other platforms should work without crash at least
