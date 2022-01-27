Debug utils label nesting
=========================

Vulkan allows applications to nest debug utils labels in *any* way with
other commands. A debug label can start outside of a render pass and end within
in or the other way around. Since we need to represent commands as a hierarchy
(think e.g. of the command viewer GUI, without this hierarchy we wouldn't
be able to collapse render passes or labels) this is a problem.
How should a command buffer record with such a command look in the gui?!

We currently fix this by just fixing the hierarchy we build internally, see
the various extra tracking and workarounds in CommandBuffer, during recording.

---

Possible issues:

### 0
A command buffer can start more debug labels than it ends.
We want to manually end the additional debug labels at the end of the record.
But remember the number of totally added labels to the stack.

### 1
A command buffer can end more debug labels than it starts.
We want to just ignore those additional commands and instead store the number of
pops the records does on the debug label stack.

### 2
a command buffer can start a debug label in a less deep nesting level than it is ended
- BeginDebugLabel
- doStuff1
- BeginRenderPass
	- doStuff2
	- EndDebugLabel
	- doStuff3
	- EndRenderPass

We want to transform that into
- BeginDebugLabel(outside)
	- doStuff1
	- EndDebugLabel(outside)
- BeginRenderPass
	- BeginDebugLabel(rp)
		- doStuff2
		- EndDebugLabel(rp)
	- doStuff3
	- EndRenderPass

Detection:
Inside EndDebugLabel, we see that the current section isn't a debug label section.
But there are still debug label sections further up the section stack.
We then insert additional dummy (outside) label commands/sections (but only
those that would be non-empty i.e. have more than one child).

### 3
A command buffer can start a debug label in deeper nesting level than it is ended:
- BeginRenderPass
	- doStuff1
	- BeginDebugLabel
	- doStuff2
	- EndRenderPass
- doStuff3
- EndDebugLabel

We want to transform that into
- BeginRenderPass
	- doStuff1
	- BeginDebugLabel(rp)
		- doStuff2
		- EndDebugLabel(rp)
	- EndRenderPass
- BeginDebugLabel(outside)
	- doStuff3
	- EndDebugLabel(outside)

Detection:
A non-debug-label section is ended even though the current section is a debug label one.
We will then first end the debug label one(s), then the non-debug-label one but
then create new dummy (outside) sections for the debug-label one (but only
those that would be non-empty).
