- rename main branch to main
- hide shader debugger behind config feature?
	- adds a whole new library and isn't ready yet at all.
	  and might never really be ready, it's more of an experiment anyways
- clean up ThreadContext (after inlining rework)
- make ThreadContext alloc and CommandRecord alloc consistent
- clean up includes to improve compile time
- allow descriptor set dereference in commandRecord, see performance.md
	- fix descriptorSet gui viewer
- only addCow the needed descriptorSets in commandHook
- capture (via addCow) the descriptorSets when selecting a record
  in the gui
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
- clean up sonar code issues
	- somehow make sure failed asserts have a [[noreturn]] addition so
	  we get less false positives for cases that we assert to be true
