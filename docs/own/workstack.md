- [ ] fix found bufparser leak
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
- [x] add mutex to descriptorPool, make sure it's correctly used everywhere
- [x] fix invalidateCbs race (see command/record.cpp TODO in tryAccessLocked)
- [ ] document what to do when descriptors are not available when
      clicking new record in UI. Implement prototype for 
	  always-cow-on-submission? Should probably be toggleable.
- [ ] clean up special descriptorSet handling in handles.cpp
- [x] allow descriptor set dereference in commandRecord, see performance.md
	- fix descriptorSet gui viewer via similar approach
	- [x] only addCow the needed descriptorSets in commandHook
	- [x] capture (via addCow) the descriptorSets when selecting a record
  in the gui
- rework gui device locking. We should be able to execute draw/uploadDraw
  without holding a lock THE WHOLE TIME. Only lock it where it's really
  needed
- [x] got the null mock driver to work.
  [x] Now write a simple test just creating an instance and device with
  vil and the validation layer. And check that we can execute that on 
  CI as well
	- write test for creating ds, updating it with imageView, destroying
	  imageView and then using ds in submission (might need partially_bound
	  or something I guess)
- rename main branch to main
- hide shader debugger behind config feature?
	- adds a whole new library and isn't ready yet at all.
	  and might never really be ready, it's more of an experiment anyways
- make ThreadContext alloc and CommandRecord alloc consistent
	- we probably also don't want to really free the blocks. Just return them.
	  Enable our global memory block cache thingy?
	- Also clean up CommandRecord alloc in general. Messy with all those
	  overloads for CommandBuffer
	- Maybe we can reuse code? The allocators are fairly similar
- look into found doom performance hotpaths
	- Get rid of getAnnotate in command buffer end. Find clean solution for
	  the problem/improve UI flickering for changing records
	- Improve QueuePresent timing
	- analyze other issues, tracy file on D:
- Clean up Handle
	- remove objectType from Handle
	- could remove 'name' from Handle, instead use HashMap in device?
	  not sure if this is a good idea though. Probably not for now.
- Clean up DeviceHandle
	- In many handles we don't need the 'dev' pointer, e.g. descriptorSet,
	  imageView etc. Remove it?
	- Instead use DeviceHandle<ObjectType>, allowing to remove objectType from Handle
	  In its destructor, pass the objectType to the destruction notification
	- descriptorSet should not derive from DeviceHandle, does not need refRecords
	- Maybe rename DeviceHandle to RecordReferenced or something?
		- split up notifyDestruction functionality in DeviceObserved or something,
		  many classes (like Fence, CommandPool etc) don't need refRecords I guess
- improve vertex viewer
	- allow to select vertices (/indices)
		- in input and output viewer
		- make rows selectable
		- render the select vertex in viewport, e.g. via point pipe
			- later: render the selected triangle?
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
