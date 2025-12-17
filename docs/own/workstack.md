- [ ] support ray tracing pipeline libraries
	- [ ] for shader patching
- [ ] try to enable bufferDeviceAddress if possible
- [ ] fix errors with validation tests
	- [ ] document how to run validation tests
- [ ] when VIL_SKIP_EXT_CHECK is set (or other env var?) override supported
      extensions in that function. Investigate how to make this work.
	  Can be provided in layer manifest or something?

- [ ] support full and+or expressions for "required" extension field
      in layer.cpp function list.
	  e.g. vkCmdSetDescriptorBufferOffsets2EXT: (vulkan1.4|maintenance6) + EXT_descriptor_buffer

- [ ] implement VK_KHR_dynamic_rendering_local_read for core 1.4
- [ ] impement VK_KHR_pipeline_executable_properties
- [ ] fix invalid pipeline barrier with BeginRendering (test e.g. with iro gpuDebugDraw)
- [ ] fix vulkan samples "Losing open cmd" with trianglevulkan13
- [ ] copyChain: add functions for proper deep copying

- [ ] Format weird for (RW)ByteAddressBuffers in hlsl/slang
	- [ ] in that case, allow to explicitly specify the format to be printed.
	- [ ] factor out entire buffmt lib into separate project?
	      would make a cool imgui binary format reader.

- [ ] properly set shaders via CmdSetShadersEXT in ComputeState/GraphicsState
      We rely on a pipeline being there in many places.
	  Maybe remove StateCmdBase::boundPipe()?
	- [ ] can test with vulkan-examples:shaderobjects sample
- [ ] Cleanup "Show All" mess in vertex viewer
	- [ ] additionally (orthogonally) have a checkbox that decides
	      whether output needs to be shown in table, be copied to host
		  Name it something like "Show Vertex Table (slow)"?
	- [ ] optimization: When "Show All" is not active, split up draw calls
	      Already outlined in vertexCopy.md

- [ ] continue device-generated-commands extension
	- [ ] test with https://github.com/nvpro-samples/vk_device_generated_cmds
	      should already run out-of-the-box (fix bugs!)
	- [ ] add indirect-command-stream reading to command hook
	- [ ] show the indirect/generated commands (with or without read-back data) in UI
- [ ] rename vku to vpp?
- [ ] Textual "bound descriptor sets" inspector
	- [ ] Allow to inspect before/after BindDescriptorSets/PushDescriptorSet
- [ ] make xfb patching also use new dynamic on-demand patching system?
	- [ ] make that less error-prone. Completely store/forward pNext?
- [ ] do we really need the advanceUntilValid calls in ds.cpp?
      We do not handle it like this for push descriptors in cb.cpp.
	  Write interfaces that can handle the overflow that vulkan allows?
	  Should be compatible with out memory layout.
- [ ] CommandHook: soft invalidate (don't invalidate state)
- [ ] add serialize support for more commands (e.g. Bind)
- [ ] serialize further gui state, e.g. selected I/O
- [ ] better deep-matching for handles, especially pipelines
- [ ] ~hook.cpp:777 TODO PERF: clear out non-reuse hooked records
- [ ] optimize acceleration structure capturing
	- optimal: when UI is not open, don't allocate on every rebuilt
- [ ] support viewing a specific array element in the buffer formatter
	- [ ] we might not even copy the needed data for some elements.
	      Only copy the needed data, we already have paging for the gui
		  but never forward what we actually need to the Hook
- [ ] support all-descriptor-indexing-flags-set, test with npt textures
	- [ ] first fix the crash (descriptor bindings not being copied should
	      not cause a crash anywhere)
	- [ ] then fix the update_unused_while_pending limitation in hook/record.cpp
		  by tracking all used handles and wait for hooked submissions before
		  destroying the api handle (optionally only track the
		  update_unused_while_pending handles? but I guess if we have the mechanism
		  we wanna use it for everything, could assert that it only happens
		  for update_unused_while_pending stuff)
			- [ ] same mechanism for memory destruction (and sparse unbinding?)
			      Have to think a bit more about the sparse handling I guess?
				  Maybe just enable the device feature that reads unbound sparse
				  regions as 0? We want that for the image viewer anyways I guess.
				  (bind memory destruction is still a problem though, handle
				  it via tracking and waiting)
- [ ] shader debugger: better handle input tab, don't invalidate selection
      every time something is changed (?), make more stable
- Vertex rework 2: electric boogaloo
	- [x] make vertex input/output styling consistent
	      Using buffmt for xfb looks kinda bad, revisit. Also table headers.
		- [x] Add ID before IDX column?
	- [ ] fix possible alignment issues in record.cpp
	- [x] Add imgui list clipper to tables and show *whole* captured data again
	- [x] figure out upside-down issue with iro. Flip y based on used viewport?
	      I guess other games just flip the viewport, that's why they did not need it
		  {solved via heuristic}
		- [~] add "y-up-mode" to options. Can be any of the 3-axis-plus-directions
			  by default for vertex input: y-up is y-up (although many models have z-up)
			  default for vertex output: y-down is y-up (except when viewport is negative,
			  then y-up is y-up).
			  {nope, kept it simple for now}
		- [~] hmm, y-flip isn't that useful atm, should rather be something
		      like oneMinusY? {nope}
	- [x] Make vertices selectable. I.e. via mouse click in table
	- [ ] show vertex table for RT displayTriangles
		- [ ] add support for vertex selection
		- [ ] Make vertices selectable for RT
	- [x] Draw selected vertex via point
		- [x] visual: ignore depth for drawn point
		- [x] visual: color it in some obvious color, red/blue
	- [x] Allow to select specific vertex (either input or output) in debugger
	- [x] Fix Recenter for top-level AccelStruct view
	- [x] Add arcball camera controls (allow both or allow to toggle via ui)
- vertex quality of life features:
	- [ ] allow selecting a triangle, highlighting the three points in the table
	- [ ] displayInstances: allow to give each instance its own color.
	      Somehow display instances? Allow to select them.
	- [ ] add hover data for vertex stuff. With exact formatted scalar
	- [ ] Allow to choose display style
		- [x] solid (single-colored or shaded) vs wireframe
		- [x] add simple triangle-normal-based lighting (sh9 based or something)
		- [ ] color using another input.
		- [x] allow not clearing background of canvas, draw on blurred ui directly?
		      looks kinda neat as well. Should probably be checkbox
		- [ ] allow to render wireframe *and* shaded view together
		- [ ] some more shading options? Allow to select a hdri and roughness?
	- [ ] allow to modify canvas size. I.e. make vertically resizeable
	- [ ] (low prio) Explicitly allow to modify what is used as position input?
	- [ ] Allow to explicitly toggle between perspective and non-perspective projection?
	- [ ] make perspective heuristic more robust, caused issues in past.
	- [ ] later: Make vertex list properly page-able, allow to see *everything*
	      without random size restrictions
	    - [x] For this to properly work with vertex input, we might need an indirect
		  	  copy (based on indirect draw command and indices.
			  See vertexCopy.md
		- [ ] For this to properly work with xfb (vertex output), we potentially
		      need to implement draw-call splitting. Which might be a pain in the ass.
- Really hard-match on vertexCount/indexCount for draw commands?
  See e.g. debug drawing in iro, adding control points to a spline will
  currently unselect the draw command. Not expected behavior.
  Maybe just match with *really* high weight.
- [ ] implement sync tracking
	- [ ] and fix full sync
	- [ ] add test for out-of-order submission
- [ ] imageViewer: add overlay showing which regions are mapped
      to which memory for sparse images
- [ ] investigate 255-overflow-like bug in shader debugger when
      resizing
- [ ] fix bad vk::name impls. E.g. for DescriptorSetLayout, the stages
- [ ] Add "Jump to End/Begin" buttons in begin/end commands.
      only show them in brokenLabel display mode?
- [ ] figure out why integration test crashes on CI.
	  execute with valgrind?
	  meson test --wrapper 'valgrind --leak-check=full --error-exitcode=1' --print-errorlogs
	  -> no idea. Crash inside the vulkan loader that i can't reproduce locally
	  Maybe just execute on windows? seems to work there.
- [ ] document what to do when descriptors are not available when
      clicking new record in UI. Implement prototype for
	  always-ds-cow-on-submission? Should probably be toggleable.
- [ ] clean up special descriptorSet handling in handles.cpp
- [ ] write integration test for creating ds, updating it with imageView, destroying
  imageView and then using ds in submission (might need partially_bound
  or something I guess)
- [ ] Don't always alloc/free in LinAllocator.
      Enable our global memory block cache thingy?

- [ ] support 1.4 core
- [ ] support VK_KHR_maintenance{5, 6} (new CmdBind calls)
- [ ] support VK_EXT_mesh_shader
	- [ ] capture vertex input via shader patching?
- [ ] properly support VK_EXT_shader_object
	- [ ] remove StateCmdBase::boundPipe()
	      Instead, introduce a ShaderState
struct ShaderStage {
	VkShaderStageFlagBits stage;
	ShaderObject* shader;
};

ShaderState {
	span<ShaderStage> stages;
	Pipeline* pipe; // optional
};


low prio:
- [ ] cleanup imageToBuffer implementation
      {NOTE: a lot better already, mainly just missing the blit implementation.
	   low prio now. See copy.cpp `enum class CopyMethod`}
	- [x] for most formats (that we can read on cpu) we probably just
	      want CmdCopyImageToBuffer
	- [ ] some formats can't be easily read on cpu. We want support on the
	      long term but there are probably always commands that won't
		  easily allow it, e.g. ASTC. For them, try
		  (1) blit to a format with at least that precision, if feature flags allow
		  (2) write into a texel buffer with at least that precision
		      need storageImageWithoutFormat device feature and the format
			  feature flag
			  In case storageImageWithoutFormat isn't supported, we
			  could have a couple of default target formats that are
			  guaranteed to be supported
			  (e.g. rgba8unorm, rgba16Sfloat, rgba32Sflot, r32Uint etc)
		  (3) if nothing else works, fall back to our old terrible
		       copy to vec4[]-storage buffer solution?
- [x] full commandbuffer/record timings.
	- [x] for this we need proper prefix-matching support in CommandHook. WIP
	- [ ] also full batch timings?
- [ ] Add "Jump to End/Begin" buttons in begin/end commands.
      only show them in brokenLabel display mode?
- [x] integration test: depend on meson subproject for mock driver
      And don't hardcode my own env path
	- [x] also make sure we don't need the layer to be installed
	      but use the latest built version.
		  pass build path in via meson config header file?
	- [x] add manual meson dependencies from integration test to used layer
	      and mock driver
	- [ ] figure out why integration test crashes on CI.
	      execute with valgrind?
		  meson test --wrapper 'valgrind --leak-check=full --error-exitcode=1' --print-errorlogs
		  -> no idea. Crash inside the vulkan loader that i can't reproduce locally
		  Maybe just execute on windows? seems to work there.
- [ ] document what to do when descriptors are not available when
      clicking new record in UI. Implement prototype for
	  always-ds-cow-on-submission? Should probably be toggleable.
- [ ] clean up special descriptorSet handling in handles.cpp
- rework gui device locking. We should be able to execute draw/uploadDraw
  without holding a lock THE WHOLE TIME. Only lock it where it's really
  needed
- [x] got the null mock driver to work.
  [x] Now write a simple test just creating an instance and device with
  vil and the validation layer. And check that we can execute that on
  CI as well
	- [ ] write test for creating ds, updating it with imageView, destroying
	  imageView and then using ds in submission (might need partially_bound
	  or something I guess)
- [x] rename main branch to main
- [ ] Don't always alloc/free in LinAllocator.
      Enable our global memory block cache thingy?
- [ ] Would be useful to have the side-by-side-frames-with-vizlcs
	  debug view via record serialization (among other things).
	  {for later}

- look into found doom performance hotpaths
	- Improve QueuePresent timing
	- analyze other issues, tracy file on D:
- check if we can get the null vulkan driver running and execute tests
	- just create images/buffers with various parameters and record submissions,
	  recording every command at least once.
	- manually test command hooking, without gui (or use gui without actually
	  rendering to a swapchain? would be nice to have that feature anyways
	  but applications that don't use a swapchain e.g. wlroots)
- clean up sonar code issues
	- somehow make sure failed asserts have a [[noreturn]] addition so
	  we get less false positives for cases that we assert to be true
- clean up includes to improve compile time
- [ ] (low prio) make keep-alive mechanism optional and implement descriptor ID housekeeping
