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
- [ ] cleanup imageToBuffer implementation
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
- Clean up Handle (?)
	- remove objectType from Handle
	- could remove 'name' from Handle, instead use HashMap in device?
	  not sure if this is a good idea though. Probably not for now.
	  our main usecase after all is application debugging where we
	  expect most handles to have a name -> embedding in object makes sense.
- Clean up DeviceHandle (?)
	- In many handles we don't need the 'dev' pointer, e.g. descriptorSet,
	  imageView etc. Remove it?
	- Instead use DeviceHandle<ObjectType>, allowing to remove objectType from Handle
	  In its destructor, pass the objectType to the destruction notification
	- descriptorSet should not derive from DeviceHandle, does not need refRecords
	- Maybe rename DeviceHandle to RecordReferenced or something?
		- split up notifyDestruction functionality in DeviceObserved or something,
		  many classes (like Fence, CommandPool etc) don't need refRecords I guess
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
