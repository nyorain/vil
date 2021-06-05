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
- external memory, fences, semaphores
- KHR_maintenance2
	- using the renderpass/imageView extensions might crash
- ui: for YCbCr conversion handles not tracked/shown
- ui: no support for variable pointers buffer

## Vulkan 1.2

Implemented:
- vkCmdDrawIndirectCount & vkCmdDrawIndexedIndirectCount
- VK_KHR_create_renderpass2
- VK_KHR_buffer_device_address (introspection support WIP)
- VK_KHR_imageless_framebuffer
- VK_EXT_descriptor_indexing
	- known problem: some edge cases with update_unused_while_pending

Missing/incomplete:
- timeline semaphores (might crash at the moment)

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
- VK_KHR_push_descriptor
- VK_KHR_descriptor_update_template
- VK_KHR_draw_indirect_count
- VK_EXT_sample_locations
- VK_EXT_discard_rectangles

There is basic support for ray tracing but no full UI support for TraceRays
commands yet:
- VK_KHR_acceleration_structure
- VK_KHR_ray_tracing_pipeline

Extensions that just add new features or flags shouldn't need any 
support by the layer but may have incomplete introspection. New functions
on physical device are usually also not a problem.
For instance:
- VK_EXT_conservative_rasterization
- VK_EXT_custom_border_color
- VK_EXT_depth_clip_enable
- VK_EXT_depth_range_unrestricted
- VK_EXT_device_memory_report
- VK_NV_cooperative_matrix

Extensions that just add SPIR-V features shouldn't be a problem in general
as long as SPIRV-Tools and spirv_reflect support it.
For instance:
- VK_KHR_ray_query
- VK_KHR_workgroup_memory_explicit_layout
- VK_KHR_zero_initialize_workgroup_memory
- VK_KHR_shader_clock
- VK_KHR_vulkan_memory_model
- VK_EXT_fragment_shader_interlock
- VK_EXT_post_depth_coverage
- VK_EXT_shader_atomic_float
- VK_EXT_shader_demote_to_helper_invocation
- VK_AMD_shader_fragment_mask
- VK_AMD_texture_gather_bias_lod
- VK_INTEL_shader_integer_functions2
- VK_NV_compute_shader_derivatives
- VK_NV_fragment_shader_barycentric
- VK_NV_geometry_shader_passthrough
- VK_NV_sample_mask_override_coverage
- VK_NV_shader_image_footprint
- VK_NV_shader_sm_builtins
- VK_NV_viewport_array2
- VK_KHR_16bit_storage
- VK_KHR_8bit_storage
- VK_KHR_shader_atomic_int64
- VK_KHR_shader_float16_int8
- VK_KHR_shader_subgroup_extended_types
- VK_KHR_spirv_1_4
- VK_KHR_storage_buffer_storage_class
- VK_KHR_variable_pointers
- VK_EXT_scalar_block_layout
- VK_EXT_shader_subgroup_ballot

## Missing extensions

Using these extensions will likely cause a crash.

- VK_KHR_synchronization2
- VK_EXT_vertex_input_dynamic_state
- VK_EXT_extended_dynamic_state2
- VK_EXT_color_write_enable
- VK_EXT_transform_feedback
- VK_AMD_buffer_marker
- VK_NV_device_diagnostic_checkpoints
- VK_NV_scissor_exclusive
- VK_NV_mesh_shader
- VK_INTEL_performance_query
- VK_NV_shading_rate_image
- VK_NV_fragment_shading_rate_enums
- VK_NV_device_generated_commands
- The old NV raytracing extensions. They also won't be supported in the future,
  use the KHR extensions instead.

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

- Extensions that have significant changes to command recording (such as
  new pipeline bind points or new commands) will very likely always lead to
  significant problems. Implementing basic support for them isn't complicated
  though
