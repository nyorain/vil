# Feature support overview

Even when a feature/extension isn't explicitly supported by the layer,
there are good chances that it might just work. If you plan on using
extensions not explicitly supported, make sure to set the `VIL_WRAP=0`
environment variable, it will significantly increase the chance that 
new extensions "just work", with a slight performance hit. 
See [env.md](env.md) for more details.

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
- VK_KHR_dedicated_allocation (not indicated in ui yet though)

Missing:
- Anything with device masks is missing; using it will very likely crash.
- no support for render pass multiview, VK_KHR_multiview
- no support for protected submit
- ui: for YCbCr conversion handles not tracked/shown
- ui: no support for variable pointers buffer
- external memory, fences, semaphores
- KHR_maintenance2
	- using the renderpass/imageView extensions might crash

## Vulkan 1.2

Implemented:
- vkCmdDrawIndirectCount & vkCmdDrawIndexedIndirectCount should not cause crashes
- VK_KHR_create_renderpass2

Missing:
- vkCmdDrawIndirectCount & vkCmdDrawIndexedIndirectCount has incomplete gui
  support, especially vertex viewer needs improvement
- timeline semaphores
- descriptor indexing: support incomplete. Viewing a command that uses
  descriptor sets with descriptor indexing may cause problems in some
  cases.
- no support for imageless_framebuffer
- no render pass extensions supported

## Non-core extensions

Extensions promoted to core not explicitly mentioned here, see above.
Supported extensions:

- VK_KHR_copy_commands2
- KHR_swapchain
- VK_EXT_debug_utils
- KHR_surface (tested overlay platforms: xlib, xcb, win32)
	- other platforms should work without crash at least

supported but no/incomplete gui interaction/information:
- VK_EXT_conditional_rendering 
- VK_EXT_extended_dynamic_state
- VK_EXT_line_rasterization
- VK_KHR_fragment_shading_rate
- VK_KHR_push_descriptor
- VK_KHR_descriptor_update_template
- VK_KHR_draw_indirect_count
- VK_EXT_sample_locations
- VK_EXT_discard_rectangles

## NOTES: 
- vendor-specific extensions are generally not a focus, unless someone
  is interested in them and writes the code or sponsors someone to do it.
- the layer does not have support for VK_EXT_debug_report/VK_EXT_debug_marker 
  and will likely not add it in the future, either, as it is deprecated. 
  Just use debug_utils, it's a better replacement.
- support VK_KHR_device_group in the near future is unlikely since making it
  work with our concepts is not trivial
- support VK_EXT_transform_feedback in the near future is unlikely as the
  layer itself is using it for the vertex viewer (which would conflict with
  an application-side use of it). It's only supposed to be used by tools/layers
  anyways.
