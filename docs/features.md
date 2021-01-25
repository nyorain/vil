# Feature support overview

## Vulkan 1.0

Missing:
- Anything with sparse/aliasing memory: not implemented/tested at all, 
  will likely cause failed assert/crash in layer

## Vulkan 1.1

Implemented:
- VK_KHR_descriptor_update_template
- VK_KHR_bind_memory2
- VK_KHR_bind_memory2
- vkCmdDispatchBase
- vkTrimCommandPool

Missing:
- Anything with device masks is missing; using it will very likely crash.
- no support for render pass multiview, VK_KHR_multiview
- no support for protected submit
- ui: for YCbCr conversion handles not tracked/shown
- ui: no support for variable pointers buffer
- external memory, fences, semaphores
- KHR_maintenance2
	- using the renderpass/imageView extensions might crash
- VK_KHR_dedicated_allocation

## Vulkan 1.2

Implemented:
- vkCmdDrawIndirectCount & vkCmdDrawIndexedIndirectCount should not cause crashes
- VK_KHR_create_renderpass2

Missing:
- vkCmdDrawIndirectCount & vkCmdDrawIndexedIndirectCount: full ui & commandHook support
- timeline semaphores
- no support for descriptor indexing
- no support for FramebufferAttachmentCreateinfo etc
- no render pass extensions supported

## Non-core extensions

Extensions promoted to core not explicitly mentioned here, see above.
Supported extensions:

- KHR_swapchain
- KHR_surface (tested overlay platforms: xlib, xcb, win32)
	- other platforms should work without crash at least
