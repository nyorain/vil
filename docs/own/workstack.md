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
- Vertex rework 2: electric boogaloo
	- [x] make vertex input/output styling consistent
	      Using buffmt for xfb looks kinda bad, revisit. Also table headers.
		- [x] Add ID before IDX column?
	- [ ] add hover data for vertex stuff. With exact formatted scalar
	- [x] Add imgui list clipper to tables and show *whole* captured data again
	- [ ] figure out upside-down issue with iro. Flip y based on used viewport?
	      I guess other games just flip the viewport, that's why they did not need it
		  {partially solved}
		- [ ] add "y-up-mode" to options. Can be any of the 3-axis-plus-directions
			  by default for vertex input: y-up is y-up (although many models have z-up)
			  default for vertex output: y-down is y-up (except when viewport is negative,
			  then y-up is y-up).
		- [ ] hmm, y-flip isn't that useful atm, should rather be something
		      like oneMinusY?
	- [ ] Make vertices selectable. I.e. via mouse click in table
	      {done only for vertex input/output for now, not RT}
	- [ ] show vertex table for RT displayTriangles
		- [ ] add support for vertex selection
	- [ ] displayInstances: allow to give each instance its own color.
	      Somehow display instances? Allow to select them.
	- [ ] Draw selected vertex via point {partially done, ugly and has issues}
		- [ ] visual: ignore depth for drawn point
		- [ ] visual: color it in some obvious color, red/blue
		- [ ] visual: make point size variable
	- [ ] allow selecting a triangle, highlighting the three points in the table
	- [x] Allow to select specific vertex (either input or output) in debugger
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
	- [x] Fix Recenter for top-level AccelStruct view
	- [ ] Allow to explicitly toggle between perspective and non-perspective projection?
	- [ ] make perspective heuristic more robust, caused issues in past.
	- [x] Add arcball camera controls (allow both or allow to toggle via ui)
	- [ ] later: Make vertex list properly page-able, allow to see *everything*
	      without random size restrictions
	    - [ ] For this to properly work with vertex input, we might need an indirect
		  	  copy (based on indirect draw command and indices.
			  See vertexCopy.md
		- [ ] For this to properly work with xfb (vertex output), we potentially
		      need to implement draw-call splitting. Which will be a pain in the ass.
- Allow to open where left off?
  Would require some serialization of frames/commands/resources.
  Also, would have to completely rework matching to work with those loaded up
  resources/commands :/ We could *never* compare for equality.
  But we probably explicitly want that in some cases, normally. So matching
  would need additional options.
- Add vertex shaders to shader debugger
	- [ ] copy vertex buffers in that case in updateHooks
	- [ ] set up vertex inputs from vertex buffers
	      Also set up stuff like VertexIndex, InstanceIndex etc.
		  All the builtin inputs
	- [ ] handle special vertex shader variables
	- [ ] important optimization: only copy the resources statically accessed by shader
- Really hard-match on vertexCount/indexCount for draw commands?
  See e.g. debug drawing in iro, adding control points to a spline will
  currently unselect the draw command. Not expected behavior.
  Maybe just match with *really* high weight.
- [ ] shader debugger: add dropdown for all embdeeded sources.
- [ ] implement sync tracking
	- [ ] and fix full sync
	- [ ] add test for out-of-order submission
- [ ] continue shader debugger
	- [ ] using some lazy copy/cow mechanism.
	      See cow.md, should probably re-introduce cows
- [ ] imageViewer: add overlay showing which regions are mapped
      to which memory for sparse images
- improve vertex viewer
	- allow to select vertices (/indices)
		- in input and output viewer
		- make rows selectable
		- render the select vertex in viewport, e.g. via point pipe
			- later: render the selected triangle?
	- implement paging
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
- [ ] full batch timings?
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
- rework gui device locking. We should be able to execute draw/uploadDraw
  without holding a lock THE WHOLE TIME. Only lock it where it's really
  needed
- [ ] write test for creating ds, updating it with imageView, destroying
  imageView and then using ds in submission (might need partially_bound
  or something I guess)
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
