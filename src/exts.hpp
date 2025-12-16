#pragma once

#include <string_view>

// Lists all supported/unsupported extensions.
// Not every extension was tested, this might contain errors. Especially
// for pure shader extensions or extensions just adding new enum values,
// they were just treated as supported.
//
// There are also some extensions that are likely false negatives
// - surface extensions for platforms we don't explicitly support/tested.
//   If you create a vil overlay via the vil api, things will
//   likely just work
// - the KHR_display (and derivative) instance extensions. Might just work.

namespace vil {

constexpr std::string_view supportedInstanceExts[] = {
	"VK_KHR_surface",
	"VK_KHR_xlib_surface",
	"VK_KHR_xcb_surface",
	"VK_KHR_wayland_surface",
	"VK_KHR_win32_surface",
	"VK_KHR_get_physical_device_properties2",
	"VK_EXT_validation_flags",
	"VK_KHR_external_memory_capabilities",
	"VK_KHR_external_semaphore_capabilities",
	"VK_KHR_external_fence_capabilities",
	"VK_KHR_get_surface_capabilities2",
	"VK_KHR_get_display_properties2",
	"VK_EXT_debug_utils",
	"VK_KHR_surface_protected_capabilities",
	"VK_EXT_headless_surface", // TODO: actively support!
	"VK_EXT_surface_maintenance1", // nop
	"VK_GOOGLE_surfaceless_query", // nop
	"VK_EXT_application_parameters", // nop
	"VK_LUNARG_direct_driver_loading", // nop
	"VK_KHR_surface_maintenance1", // nop
};

constexpr std::string_view unsupportedInstanceExts[] = {
	"VK_KHR_display",
	"VK_KHR_mir_surface",
	"VK_KHR_android_surface",
	"VK_GGP_stream_descriptor_surface",
	"VK_NN_vi_surface",
	"VK_KHR_device_group_creation",
	"VK_MVK_ios_surface",
	"VK_MVK_macos_surface",
	"VK_MVK_moltenvk",
	"VK_FUCHSIA_imagepipe_surface",
	"VK_EXT_metal_surface",
	"VK_EXT_acquire_drm_display",
	"VK_EXT_directfb_surface",
	"VK_QNX_screen_surface",
	"VK_NV_display_stereo",
	"VK_OHOS_surface",

	"VK_NV_external_memory_capabilities",
	"VK_EXT_direct_mode_display",
	"VK_EXT_acquire_xlib_display",
	"VK_EXT_display_surface_counter",
	"VK_KHR_portability_enumeration",

	"VK_EXT_debug_report", // TODO: support this for legacy apps!
};

constexpr std::string_view supportedDevExts[] = {
	// explicitly supported
	"VK_KHR_swapchain",
	"VK_KHR_dynamic_rendering",
	"VK_KHR_push_descriptor",
	"VK_EXT_conditional_rendering",
	"VK_KHR_descriptor_update_template",
	"VK_EXT_discard_rectangles",
	"VK_KHR_imageless_framebuffer",
	"VK_KHR_create_renderpass2",
	"VK_EXT_inline_uniform_block",
	"VK_EXT_sample_locations",
	"VK_KHR_acceleration_structure",
	"VK_KHR_ray_tracing_pipeline", // TODO vkCmdTraceRaysIndirectKHR support incomplete
	"VK_KHR_bind_memory2",
	"VK_EXT_image_drm_format_modifier",
	"VK_EXT_descriptor_indexing",
	"VK_KHR_maintenance3",
	"VK_AMD_draw_indirect_count", // core now
	"VK_KHR_draw_indirect_count",
	"VK_KHR_timeline_semaphore",
	"VK_KHR_fragment_shading_rate",
	"VK_EXT_buffer_device_address",
	"VK_KHR_present_wait",
	"VK_KHR_buffer_device_address",
	"VK_EXT_line_rasterization",
	"VK_KHR_map_memory2",
	"VK_EXT_map_memory_placed",
	"VK_KHR_pipeline_library",
	"VK_KHR_synchronization2",
	"VK_EXT_graphics_pipeline_library",
	"VK_EXT_mesh_shader",
	"VK_KHR_copy_commands2",
	"VK_EXT_vertex_input_dynamic_state",
	"VK_EXT_color_write_enable",
	"VK_EXT_multi_draw",
	"VK_EXT_pageable_device_local_memory",
	"VK_EXT_extended_dynamic_state",
	"VK_EXT_extended_dynamic_state2",
	"VK_EXT_extended_dynamic_state3",
	"VK_KHR_maintenance5",
	"VK_EXT_shader_object",
	"VK_KHR_line_rasterization",
	"VK_KHR_maintenance6",
	"VK_EXT_depth_clamp_control",
	"VK_KHR_maintenance9",

	// likely supported, mostly no-ops for vil itself
	"VK_EXT_depth_range_unrestricted",
	"VK_KHR_sampler_mirror_clamp_to_edge",
	"VK_IMG_filter_cubic",
	"VK_AMD_rasterization_order",
	"VK_AMD_shader_trinary_minmax",
	"VK_AMD_shader_explicit_vertex_parameter",
	"VK_AMD_gcn_shader",
	"VK_AMD_negative_viewport_height",
	"VK_AMD_gpu_shader_half_float",
	"VK_AMD_shader_ballot",
	"VK_AMD_texture_gather_bias_lod",
	"VK_AMD_shader_image_load_store_lod",
	"VK_NV_corner_sampled_image",
	"VK_KHR_shader_draw_parameters",
	"VK_EXT_shader_subgroup_ballot",
	"VK_EXT_shader_subgroup_vote",
	"VK_EXT_texture_compression_astc_hdr",
	"VK_EXT_astc_decode_mode",
	"VK_KHR_maintenance1",
	"VK_KHR_external_memory",
	"VK_KHR_external_memory_win32",
	"VK_KHR_external_memory_fd",
	"VK_KHR_win32_keyed_mutex",
	"VK_KHR_external_semaphore",
	"VK_KHR_external_semaphore_win32",
	"VK_KHR_external_semaphore_fd",
	"VK_KHR_shader_float16_int8",
	"VK_KHR_16bit_storage",
	"VK_KHR_incremental_present", // TODO: might make problems with overlay
	"VK_NV_clip_space_w_scaling",
	"VK_NV_sample_mask_override_coverage",
	"VK_NV_geometry_shader_passthrough",
	"VK_EXT_conservative_rasterization",
	"VK_EXT_depth_clip_enable",
	"VK_EXT_swapchain_colorspace",
	"VK_EXT_hdr_metadata",
	"VK_KHR_shared_presentable_image",
	"VK_KHR_external_fence",
	"VK_KHR_external_fence_win32",
	"VK_KHR_external_fence_fd",
	"VK_KHR_maintenance2",
	"VK_KHR_variable_pointers",
	"VK_EXT_external_memory_dma_buf",
	"VK_EXT_queue_family_foreign", // TODO: might cause issues with our barrier insertion?
	"VK_KHR_dedicated_allocation",
	"VK_EXT_sampler_filter_minmax",
	"VK_KHR_storage_buffer_storage_class",
	"VK_AMD_gpu_shader_int16",
	"VK_AMD_mixed_attachment_samples",
	"VK_AMD_shader_fragment_mask",
	"VK_EXT_shader_stencil_export",
	"VK_KHR_relaxed_block_layout",
	"VK_KHR_get_memory_requirements2",
	"VK_KHR_image_format_list",
	"VK_EXT_blend_operation_advanced",
	"VK_NV_fragment_coverage_to_color",
	"VK_KHR_ray_query",
	"VK_NV_framebuffer_mixed_samples",
	"VK_NV_fill_rectangle",
	"VK_NV_shader_sm_builtins",
	"VK_EXT_post_depth_coverage",
	"VK_KHR_sampler_ycbcr_conversion",
	"VK_EXT_validation_cache", // TODO: actively use!
	"VK_EXT_shader_viewport_index_layer",
	"VK_EXT_filter_cubic",
	"VK_QCOM_render_pass_shader_resolve",
	"VK_EXT_global_priority",
	"VK_KHR_shader_subgroup_extended_types",
	"VK_KHR_8bit_storage",
	"VK_EXT_external_memory_host",
	"VK_KHR_shader_atomic_int64",
	"VK_KHR_shader_clock",
	"VK_AMD_pipeline_compiler_control",
	"VK_EXT_calibrated_timestamps",
	"VK_AMD_shader_core_properties",
	"VK_AMD_memory_overallocation_behavior",
	"VK_EXT_pipeline_creation_feedback",
	"VK_KHR_driver_properties",
	"VK_KHR_shader_float_controls",
	"VK_NV_shader_subgroup_partitioned",
	"VK_KHR_swapchain_mutable_format",
	"VK_NV_compute_shader_derivatives",
	"VK_NV_fragment_shader_barycentric",
	"VK_NV_shader_image_footprint",
	"VK_EXT_present_timing", // TODO only when swapchain isn't wrapped.
	"VK_INTEL_shader_integer_functions2",
	"VK_KHR_vulkan_memory_model",
	"VK_EXT_pci_bus_info",
	"VK_AMD_display_native_hdr",
	"VK_AMD_display_native_hdr", // only when swpachain isnt wrapped
	"VK_KHR_shader_terminate_invocation",
	"VK_EXT_fragment_density_map",
	"VK_EXT_scalar_block_layout",
	"VK_GOOGLE_hlsl_functionality1",
	"VK_GOOGLE_decorate_string",
	"VK_EXT_subgroup_size_control",
	"VK_AMD_shader_core_properties2",
	"VK_AMD_device_coherent_memory",
	"VK_EXT_shader_image_atomic_int64",
	"VK_KHR_shader_quad_control",
	"VK_KHR_spirv_1_4",
	"VK_EXT_memory_budget",
	"VK_EXT_memory_priority",
	"VK_NV_dedicated_allocation_image_aliasing",
	"VK_KHR_separate_depth_stencil_layouts", // not 100% sure about rp splitting
	"VK_EXT_tooling_info", // TODO: actively implement!
	"VK_EXT_separate_stencil_usage",
	"VK_EXT_validation_features",
	"VK_NV_cooperative_matrix",
	"VK_NV_coverage_reduction_mode",
	"VK_EXT_fragment_shader_interlock",
	"VK_EXT_ycbcr_image_arrays",
	"VK_KHR_uniform_buffer_standard_layout",
	"VK_EXT_provoking_vertex",
	"VK_EXT_full_screen_exclusive",
	"VK_EXT_shader_atomic_float",
	"VK_EXT_host_query_reset", // TODO only when QueryPool not wrapped
	"VK_EXT_index_type_uint8", // supported for all hooking/gui ops?
	"VK_KHR_deferred_host_operations", // nop support
	"VK_EXT_shader_atomic_float2",
	"VK_EXT_swapchain_maintenance1", // TODO: only when swapchain not wrapped
	"VK_EXT_shader_demote_to_helper_invocation",
	"VK_NV_inherited_viewport_scissor",
	"VK_KHR_shader_integer_dot_product",
	"VK_EXT_texel_buffer_alignment",
	"VK_QCOM_render_pass_transform",
	"VK_EXT_depth_bias_control",
	"VK_EXT_device_memory_report",
	"VK_EXT_robustness2",
	"VK_EXT_custom_border_color",
	"VK_GOOGLE_user_type",
	"VK_NV_present_barrier",
	"VK_KHR_shader_non_semantic_info",
	"VK_KHR_present_id",
	"VK_EXT_pipeline_creation_cache_control",
	"VK_NV_device_diagnostics_config",
	"VK_QCOM_render_pass_store_ops",
	"VK_NV_low_latency",
	"VK_AMD_shader_early_and_late_fragment_tests",
	"VK_KHR_fragment_shader_barycentric",
	"VK_KHR_shader_subgroup_uniform_control_flow",
	"VK_KHR_zero_initialize_workgroup_memory",
	"VK_EXT_ycbcr_2plane_444_formats",
	"VK_EXT_fragment_density_map2",
	"VK_QCOM_rotated_copy_commands",
	"VK_EXT_image_robustness",
	"VK_KHR_workgroup_memory_explicit_layout",
	"VK_EXT_attachment_feedback_loop_layout",
	"VK_EXT_4444_formats", // no cpu formatting support
	"VK_EXT_device_fault",
	"VK_ARM_rasterization_order_attachment_access",
	"VK_EXT_rgba10x6_formats", // no cpu formatting support
	"VK_EXT_physical_device_drm",
	"VK_EXT_device_address_binding_report",
	"VK_EXT_depth_clip_control",
	"VK_EXT_primitive_topology_list_restart",
	"VK_KHR_format_feature_flags2",
	"VK_EXT_present_mode_fifo_latest_ready",
	"VK_EXT_primitives_generated_query",
	"VK_KHR_ray_tracing_maintenance1",
	"VK_KHR_shader_untyped_pointers",
	"VK_EXT_global_priority_query",
	"VK_EXT_image_view_min_lod",
	"VK_EXT_image_2d_view_of_3d",
	"VK_EXT_shader_tile_image",
	"VK_EXT_load_store_op_none",
	"VK_EXT_border_color_swizzle",
	"VK_KHR_maintenance4",
	"VK_ARM_shader_core_properties",
	"VK_KHR_shader_subgroup_rotate",
	"VK_ARM_scheduling_controls",
	"VK_EXT_image_sliced_view_of_3d",
	"VK_EXT_depth_clamp_zero_one",
	"VK_EXT_non_seamless_cube_map",
	"VK_QCOM_fragment_density_map_offset",
	"VK_NV_linear_color_attachment",
	"VK_KHR_shader_maximal_reconvergence",
	"VK_EXT_image_compression_control_swapchain",
	"VK_QCOM_image_processing",
	"VK_EXT_nested_command_buffer",
	"VK_EXT_external_memory_acquire_unmodified",
	"VK_EXT_shader_module_identifier", // TODO: only when shaderModule not wrapped
	"VK_EXT_rasterization_order_attachment_access",
	"VK_EXT_legacy_dithering",
	"VK_EXT_pipeline_protected_access",
	"VK_AMD_anti_lag",
	"VK_KHR_present_id2",
	"VK_KHR_present_wait2", // TODO only when swapchain not wrapped
	"VK_KHR_ray_tracing_position_fetch",
	"VK_KHR_swapchain_maintenance1", // TODO: only when swapchain not wrapped
	"VK_NV_ray_tracing_invocation_reorder",
	"VK_NV_extended_sparse_address_space",
	"VK_EXT_legacy_vertex_attributes",
	"VK_EXT_layer_settings", // TODO actively use
	"VK_ARM_shader_core_builtins",
	"VK_EXT_pipeline_library_group_handles", // TODO: rt pipe patching still working?
	"VK_EXT_dynamic_rendering_unused_attachments",
	"VK_NV_low_latency2", // TODO not 100% sure
	"VK_KHR_cooperative_matrix",
	"VK_KHR_compute_shader_derivatives",
	"VK_QCOM_image_processing2",
	"VK_QCOM_filter_cubic_weights",
	"VK_QCOM_ycbcr_degamma",
	"VK_QCOM_filter_cubic_clamp",
	"VK_KHR_load_store_op_none",
	"VK_KHR_unified_image_layouts",
	"VK_KHR_shader_float_controls2",
	"VK_MSFT_layered_driver",
	"VK_KHR_index_type_uint8",
	"VK_KHR_calibrated_timestamps",
	"VK_KHR_shader_expect_assume",
	"VK_NV_descriptor_pool_overallocation",
	"VK_NV_raw_access_chains",
	"VK_KHR_shader_relaxed_extended_instruction",
	"VK_KHR_maintenance7",
	"VK_NV_shader_atomic_float16_vector",
	"VK_EXT_shader_replicated_composites",
	"VK_EXT_shader_float8",
	"VK_NV_ray_tracing_validation",
	"VK_EXT_device_generated_commands", // TODO incomplete, test and show in UI
	"VK_KHR_maintenance8",
	"VK_MESA_image_alignment_control",
	"VK_KHR_shader_fma",
	"VK_EXT_ray_tracing_invocation_reorder",
	"VK_NV_cooperative_matrix2",
	"VK_KHR_depth_clamp_zero_one",
	"VK_EXT_vertex_attribute_robustness",
	"VK_ARM_format_pack",
	"VK_KHR_robustness2",
	"VK_NV_present_metering",
	"VK_EXT_zero_initialize_device_memory",
	"VK_KHR_present_mode_fifo_latest_ready",
	"VK_EXT_shader_64bit_indexing",
	"VK_EXT_shader_uniform_buffer_unsized_array",
	"VK_VALVE_mutable_descriptor_type",
	"VK_EXT_mutable_descriptor_type", // TODO: push + mutable not supported right now
	"VK_KHR_depth_stencil_resolve",
	"VK_EXT_vertex_attribute_divisor",
	"VK_KHR_vertex_attribute_divisor",
};

// Known/look like they might cause problems or crashes.
constexpr std::string_view unsupportedDevExts[] = {
	"VK_NVX_binary_import",
	"VK_NVX_image_view_handle",
	"VK_EXT_transform_feedback",
	"VK_EXT_debug_marker",
	"VK_ANDROID_native_buffer",
	"VK_NV_glsl_shader",
	"VK_KHR_video_queue",
	"VK_KHR_video_decode_queue",
	"VK_NV_dedicated_allocation",
	"VK_EXT_video_encode_h264",
	"VK_EXT_video_encode_h265",
	"VK_EXT_video_decode_h264",
	"VK_AMD_shader_info", // TODO: easy to support and useful!
	"VK_KHR_multiview", // might need non-trivial RenderPass splitting changes
	"VK_IMG_format_pvrtc",
	"VK_NV_external_memory",
	"VK_NV_external_memory_win32",
	"VK_NV_win32_keyed_mutex",
	"VK_KHR_device_group",
	"VK_EXT_display_control",
	"VK_GOOGLE_display_timing",
	"VK_NV_viewport_array2",
	"VK_NVX_multiview_per_view_attributes",
	"VK_NV_viewport_swizzle",
	"VK_KHR_performance_query", // TODO: would be nice to support and use!
	"VK_ANDROID_external_memory_android_hardware_buffer",
	"VK_KHR_portability_subset", // TODO: nice to have!
	"VK_NV_ray_tracing", // only KHR ray tracing is supported
	"VK_NV_shading_rate_image",
	"VK_AMD_buffer_marker", // TODO: just new cmd command!
	"VK_EXT_video_decode_h265",
	"VK_GGP_frame_token",
	"VK_NV_mesh_shader",
	"VK_NV_scissor_exclusive", // new command
	"VK_NV_device_diagnostic_checkpoints", // new command
	"VK_INTEL_performance_query", // new command
	"VK_KHR_dynamic_rendering_local_read", // TODO: 1.4 core
	"VK_KHR_pipeline_executable_properties", // TODO: also show props in GUI
	"VK_EXT_host_image_copy",
	"VK_NV_device_generated_commands",
	"VK_EXT_private_data", // TODO: will likely crash quickly
	"VK_KHR_video_encode_queue",
	"VK_NV_cuda_kernel_launch",
	"VK_KHR_object_refresh", // commands
	"VK_QCOM_tile_shading", // commands
	"VK_EXT_metal_objects",
	"VK_EXT_descriptor_buffer", // TODO!
	"VK_NV_fragment_shading_rate_enums", // command
	"VK_NV_ray_tracing_motion_blur", // might work as nop?
	"VK_EXT_image_compression_control", // image wrapped
	"VK_NV_acquire_winrt_display",
	"VK_FUCHSIA_external_memory",
	"VK_FUCHSIA_external_semaphore",
	"VK_FUCHSIA_buffer_collection",
	"VK_HUAWEI_subpass_shading", // command
	"VK_HUAWEI_invocation_mask", // command
	"VK_NV_external_memory_rdma",
	"VK_EXT_pipeline_properties", // TODO
	"VK_NV_external_sci_sync",
	"VK_NV_external_memory_sci_buf",
	"VK_EXT_frame_boundary", // handle unwrapping. TODO: use!
	"VK_EXT_multisampled_render_to_single_sampled",
	"VK_VALVE_video_encode_rgb_conversion",
	"VK_EXT_opacity_micromap",
	"VK_NV_displacement_micromap",
	"VK_HUAWEI_cluster_culling_shader",
	"VK_VALVE_descriptor_set_host_mapping", // TODO
	"VK_ARM_render_pass_striped",
	"VK_NV_copy_memory_indirect", // commands
	"VK_NV_memory_decompression", // commands
	"VK_NV_device_generated_commands_compute",
	"VK_NV_ray_tracing_linear_swept_spheres",
	"VK_OHOS_external_memory",
	"VK_EXT_subpass_merge_feedback", // struct pointer patching
	"VK_ARM_tensors",
	"VK_NV_optical_flow", // command
	"VK_ANDROID_external_format_resolve",
	"VK_AMDX_dense_geometry_format",
	"VK_KHR_pipeline_binary", // TODO
	"VK_QCOM_tile_properties",
	"VK_SEC_amigo_profiling",
	"VK_QCOM_multiview_per_view_viewports",
	"VK_NV_external_sci_sync2",
	"VK_NV_cooperative_vector",
	"VK_ARM_data_graph",
	"VK_QCOM_multiview_per_view_render_areas",
	"VK_KHR_video_decode_av1",
	"VK_KHR_video_encode_av1",
	"VK_KHR_video_decode_vp9",
	"VK_KHR_video_maintenance1",
	"VK_NV_per_stage_descriptor_set",
	"VK_EXT_attachment_feedback_loop_dynamic_state", // TODO
	"VK_QNX_external_memory_screen_buffer",
	"VK_QCOM_tile_memory_heap",
	"VK_KHR_copy_memory_indirect", // TODO commands
	"VK_EXT_memory_decompression", // TODO commands
	"VK_KHR_video_encode_intra_refresh",
	"VK_KHR_video_encode_quantization_map",
	"VK_NV_external_compute_queue",
	"VK_NV_command_buffer_inheritance", // would need huge rework of cb state
	"VK_NV_cluster_acceleration_structure",
	"VK_NV_partitioned_acceleration_structure",
	"VK_KHR_video_maintenance2",
	"VK_OHOS_native_buffer",
	"VK_HUAWEI_hdr_vivid",
	"VK_ARM_pipeline_opacity_micromap",
	"VK_EXT_external_memory_metal",
	"VK_ARM_performance_counters_by_region",
	"VK_VALVE_fragment_density_map_layered",
	"VK_EXT_fragment_density_map_offset", // command
	"VK_EXT_custom_resolve", // commands
	"VK_QCOM_data_graph_model",
	"VK_KHR_maintenance10", // TODO: CmdEndRendering2
	"VK_SEC_pipeline_cache_incremental_mode",
};

// last VK_EXT_image_compression_control

} // namespace
