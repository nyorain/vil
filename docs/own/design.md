# Design

This document includes some random notes and thoughts and docs done
while implementing various features. Most of it might be outdated,
don't rely on it.

Synchronization
================

There is a mutex for global data (e.g. the hash table associating
VkDevice/VkInstance with our structures). It is only used for that.

The most important mutex is probably the per-device mutex. Since we track
all resources created for a device (and resources can be created/destroyed from
any thread, basically), we use it a lot for resource creation and destruction.
It used for more than just adding/removing from the set of resource though:
There are a lot of instances where we can't perform synchronization on
per-resource handle on destruction. For instance consider this situation:

There is an N-to-M relationship between resources of type A and type B.
That means, every type A resource is associated/connected to a number of type B
resources and every resource of type B to a number of type A resources.
In many cases we have to track all of these connections.
(If it helps you think e.g. of A = Image and B = CommandBuffer but
there are many cases where vulkan behaves something like this).
Now, vulkan usually allows resources to be destroyed from any thread.
And vulkan does not require a certain destruction order for many resources
(meaning you can destroy a memory before the buffer bound to it as long
as you don't use the buffer after destroying its memory).
So we might have a situation (that seems valid to me per spec) where
one thread destroys a resource R_a of type A and at the same time another
thread destroys a resource R_b of type B that is connected to R_a.
So the destruction of R_a must signal R_b that it was destroyed (and remove
itself from the list of connected resources) while the destruction of
R_b vice versa. This case (and possibly other, related cases; some of them
possibly even simpler) are extremely hard to synchronize correctly on a 
per-object basis. The native way (during destruction of a resource, first
lock their own mutex, then unregister from all connected resources, locking
each of their mutexes nestedly) may result in a deadlock in the situation
above, for instance.

That's why we serialize resource destruction (and also similar
operations like invalidation).


?
Due to the way we structure our critical sections, there may be a state in
which a handle is still valid (and not yet removed from all connections)
but already removed from the devices resource list. So don't assume that
all handles referenced somewhere must be present in the device's list
of resources.

If that is ever a problem, could add something like 
```
// Function that is called when this handle is removed from the
// device list of resources. Will be called while the device mutex is locked.
void DeviceHandle::preDestroyDevLocked();
```

---

In general, member of a handle can be
- private, synchronized in vulkan API implementations just via vulkan
  guarantees
- public and immutable (like e.g. create info)
- public and mutable; synchronized over a mutex (usually device mutex)

# On synchronization and resource lifetimes:

- the application might destroy a vulkan object while we are currently
  displaying information about it in the overlay renderer. How to
  handle that?
  	- first try was to require handle destructors to be called while 
	  the device mutex is locked (we already need to do that for
	  removing it from the device's resource list) and informing
	  all renderers about it

But the problem isn't restricted to renderers. Any handle may reference
another handle. And often enough vulkan allows to destroy handle A even
though B still references it, e.g. you can destroy an image before a
framebuffer it is used in (couldn't find anything in vulkan saying it's not
allowed at least, validation layers don't warn; even though that obviously 
makes the framebuffer invalid and you can only destroy it afterwards). 
Especially for command buffers (where we might want to display the state
even if they are invalid by a referenced resource being destroyed/changed)
this is a problem.

Second try:
Just use shared pointers where resources are referenced. Less critical sections,
keeping our resource information alive for debugging purposes is *exactly* what we want.
Example:
- In a Framebuffer, we store its attachment at shared_ptr<Image> instead of just Image*
- In Image, we want to know all framebuffers it's used in as well. But we are not
  interested in the framebuffers it was previously used in. That's why we simply
  use Framebuffer* here.
This means, when destroying a framebuffer we inform all associated images that
the framebuffer was destroy (which is natural since framebuffer builds on top
of image) but when an image is destroyed, we never have to inform any
framebuffers (which was weird to do in the first place).

Why (not) shared ownership
==========================

We use shared ownership for all handles inside the layer for multiple reasons:
- when not doing so we would have to do a lot more tracking manually.
  For instance, we want to show the image belonging to an image view in
  the overlay, allowing to directly jump to inspect it. But an image may
  be legally destroyed before an image view. So we would have to explicitly
  unregister the image from all image views on destruction (but this is
  not even what we want, if the image view is still alive we want to get
  information about the image it was created with, even if that was destroyed).
  	- Well, the example with image and image view isn't entirely clear. It's
	  much more common with command buffers: Most vulkan applications have
	  at one point command buffers in invalid state laying around (that won't
	  be used like that anymore but re-recorded before the next use). How
	  should the overlay handle them? Don't show them at all? Show them but
	  just display them as "invalid"? But we actually want to display a history
	  of submitted vulkan commands.

Ok, wait, do we really? Is displaying a history worth all the struggle?
Just displaying the command buffer as "being in invalid state so we don't
show its contents" sounds pretty sensible to me.

In the end, we are not trying to go for full shared ownership for all handles
(it still makes sense for information that are truly shared and not owned
by a specific handle/might outlive it like spirv shader module or renderpass
data).

Why no typesafe C++ vulkan wrapper?
===================================

Using vulkan can be made a lot easier by just using a typesafe C++ wrapper,
avoiding errors due to missing sType or pNext initialization as well as
passing wrong enum/flag values. But, while it is easy in an application
to use such a wrapper, it's hard to use in a layer: We receive C-API
handles, structs and enums and have to pass them on a C-type handles
(simply casting function pointers is a no-go to since it's undefined behavior).
Therefore, we don't use it here.
Early version of this layer tried to use it but that resulted just in
weird situations and additional cast and an additional dependency.

# On vulkan host allocators

A known issue of the VIL design: host allocators might not
work a 100% correctly in all cases, consider this case:
We have to keep a handle alive inside the layer while the application
logically destroyed it. When we destroy the handle later on (after
the vkDestroy* call has returned to the application), we can no longer
use the application-supplied allocator (since that might be
unexpected for the application. The vulkan seems does not seem to
clearly forbid it but it's probably meant to be? we would also have
to take care from which thread the handle is destroyed in the end).
Therefore, we completely ignore allocators for those handles (mainly layout
handles at the moment).
We could optionally simply use our own allocators *everywhere*, might
be a useful thing to do anyways (but allow to enable/disable it via env var for
instance).

# Hooking command buffers

Hooking a command buffer means to have a modified version of it ourselves
that we pass down the function chain. This way, we can modify, insert
or erase commands. Each time a command buffer is submitted, we check whether
it is hooked and if so retrieve the command buffer to pass down the chain
from the hooking entity (you could call it hooker, harr, harr).

A command buffer can only have one hook installed at a time.
What happens if we e.g. have multiple overlays trying to modify it at
the same time? Probably ok if it just does not work. Maybe we don't want
to support multiple active overlays anyways, might have problems down the line.

# Event log/tracing concept

```
struct TracedEvent {
	enum class Type {
		submit = (1u << 0), // queue submission/sparse binding
		present = (1u << 1), // vkQueuePresentKHR call
		creation = (1u << 2), // creation of a new resource
		destruction = (1u << 3), // destruction of a resource
	};

	// Display the event via ImGui
	virtual void display() const = 0;
	virutal Type type() const = 0;
};

struct SubmissionEvent {
	Queue* queue;

	// somehow track it?
};

struct CreationEvent {
	Handle* handle;
};

struct DestructionEvent {
	Handle* handle;
};

struct EventGui {
};
```

# On internal assumptions

In our vulkan api implementation we just assume that we know handles
that are passed to us. This might not be always true in practice. Consider:
- Some day, there is a a VK_KHR_lit_renderpass extension that adds a
  new method `vkCreateLitRenderpassKHR` to create totally new, awesome 
  renderpasses and we don't support this extension (yet). 
  When an application uses this extension it creates a renderpass that isn't
  known to us, which will lead to a failed assertion when this renderpass
  is used in functions we do hook (we will call dev.renderpasses.get(rp)
  which won't find it and output an assertion). But we will furthermore
  not check whether we found this renderpass and just assume we did,
  i.e. we will most likely crash inside the layer.

Since not all extensions are supported by us yet and we will likely not support
extensions right when they come out, this is kinda bad. But this was done
deliberately, the alternative would be worse. The alternative would be to
have *a lot* of additional checks and branches and make the code *a lot*
less maintainable overall. In the end, it should not be hard at all to
add support for the extension __to a degree that our assumption is met__.
Which means pretty much just supported new creation functions. That's it.
So if an extension someone wants to use isn't supported by us, we can
add support for it, usually very quickly and easily (obviously, extending
our debugging/profiling capabilities to cover the extension is a lot more
work and might come later on).

So this decision was done to keep the code clean and as a motivation to just
implement quick-n-dirty support for all extensions as quickly as possible.
A downside that has to be kept in mind: even if we manage to maintain the
layer that well, it might take a while before it arrives downstream. (But,
usually, so do new vulkan versions & extensions. Hardcore users that
quickly update for new extensions should be able to update the layer quickly
as well/build it themselves from main).

# Window input hooking

This one shouldn't even be officially supported, it's such a hack. Anyways,
useful as a super-quick dropin. When applications don't explicitly use the
vil api to pass input to the layer (for gui stuff) we can atempt to hijack
the windows input using platform-specific APIs. Yes, this is super ugly
and super evil and not guaranteed to work at all, will just ignore bugs
regarding that. 

Getting input isn't the hard part, it's doable on all platforms. The hard
part is finding a consistent implementation for all backends of what input
is blocked from application and what input is not. Main backends:

- X11. Unsurprisingly, ugly as hell. But also, pretty flexible, allows us
  to do all the ugliness the want. We can't directly hijack input (could do
  so with general window-unrelated APIs but this is complicated, leads to
  new problems and does not allow us to block it) but we can create an
  InputOnly child window.
  Even without that, we can check the current focus window and key states,
  allowing us to detect a key to toggle the overlay.
  	- One difficulty is threading with X11. We don't really need a thread tho.
	  For x11 but most vulkan drivers do and applications not calling XInitThreads
	  are undefined anyways.
- Wayland. We can directly hook input but can't block it like that.
  On some implementations (tested: weston), subsurfaces receive focus and
  block input from the parent. That's the way I'd understand the wayland
  spec and likely the expected & intended way. With some compositors this does
  not work though (namely: sway, maybe it's a wlroots problem extending
  to other compositors). See https://github.com/swaywm/sway/issues/5904
  We might be able to reliably achieve this using xdg popup though. This might
  result in the input capture being closed every time focus is moved from
  the application but that's ok I guess.
- Win32. Not implemented yet. We can likely achieve it via a child window as well
  using the layered and transparent flags, making it fully transparent

Features that are neat to have:

- open the overlay via a pressed key
	- also close it again (via that key)
  This one is actually the easiest to achieve. We don't have to block any
  input from reaching the actual application.
- when overlay is open, capture input to allow user interaction with overlay
- allow somehow giving back control to the application while still leaving
  the overlay open. Probably best done via key. But would be neat if this
  could be triggered when mouse is clicked outside of overlay (and then
  the next overlay shortcut will simply give it focus again, not hide it).

## Command Buffer Groups

Often, command buffers over multiple frames have *almost* the same content.
It's just that every frame contains some new objects, removes rendering
of some old objects, potentially even add/removes whole render passes.
It should be possible to inspect this logical group of command buffers
continuously, even if they are rerecorded in every frame.

- when a command buffer is submitted, we check the situation against all
  existing command groups: is its contents similar enough in structure
  (we explicitly want to ignore slightly different draw commands & descriptor
  sets, different framebuffers etc to a certain extent; we should mainly match
  for general data flow structure, CommandDescription is a good start).
  We also compare the queue used to submit it (and possibly the command pool).
  When we detect that it's part of a command group, add it there.
  (Commands will less than N commands should probably never be part of a command
  group. Same for commands with no structure (i.e. no labels/renderpasses?)).
- When a command group is empty, remove it (also do this when no cb from a
  group was submitted for N frames/time? probably not a good idea)
- The command buffer viewer can then also display command groups (and we always
  link to command groups in the list of submissions to avoid the flickering
  of a new command buffer in every frame). What do to when the structure/the 
  commands is different somewhere?
  	- we could explicitly show this in command view and offer a selection
	  to only show commands from one of the command buffers in that region
	- extend the idea above: toggle display of each command buffer in the view.
	  We display the union of them (still trying to merge what can be merged).
	  
# Performance

First performance tests on a real game showed: basic resource tracking is
enough to completely mess up frame times! Couple of reasons:

- completely underestimated the number of resources in sample cases:
  a descriptor set can reference many thousand resources and a resource can
  be referenced in many thousand descriptor sets. Linear times for creation
  and destruction are not acceptable.
- completely underestimated the number of commands in a command buffer.
  Games can easily have thousands of draw calls (where each draw call previously
  has multiple bind commands) and, as it turns out, allocating memory 
  in __EVERY SINGLE ONE OF THEM__ is a bad idea. Also, games record command
  buffers in every frame, in practice (we at least have to be ready
  for this case).

EDIT: a lot of the performance problems came from locking the mutex
too long when rendering ui.
But also from our handling of `vkCmdBindDescriptorSets`. We simply cannot
afford to iterate through all bindings in a descriptor set, games might
use bindless techniques (even without the descriptor indexing extension,
a descriptor set might hold hundreds/thousands of images for instance).
We simply store the descriptor sets. And when someone is interested in the
images used by a command buffer, for instance, they also have to iterate
over the used descriptor sets. But this query isn't really ever needed.
Much rather we want to see if, for a given handle, there is a command buffer
that uses it. And we can do that via the handle->refCbs and
handle->descriptors->refCbs referenced command buffer sets.

# On Vertex Viewing

Getting vertex processing insights, such as in renderdoc, is actually
fairly complicated.

1. The input can simply be read from bound index/vertex buffers directly,
   this is the easy part. But nowadays, vertex shader can use storage
   buffers/images/whatever perfectly well and not only rely on vertex input,
   so...
2. As renderdoc, we want - and need - to show the output of the vertex
   stage. This is difficult. Options are:
   	- Use transform feedback. Might not be supported everywhere, really
	  ugly and in conflict with other things the application might do
	- Transform the vertex shader into a compute shader that writes its
	  output into an SSBO inside the layer and let that run to get output.
	  Lots of work, modifying spirv. Might be error-prone as well, think
	  of complicated use-cases such as multiview renderpasses.
	- modify the vertex shaders to (optionally) write out stuff to a
	  ssbo. The spirv changes should be minimal. Could be problematic as we 
	  need an additional descriptor though.
3. The problems don't stop with the vertex stage. To allow debugging
   of geometry and tessellation stages, we need their output as well.
   Ugh. Re-implementing that logic in compute shaders is a bad idea.
   We likely just need to use transform feedback here.

We don't really need vkCmdDrawIndirectByteCountEXT, we all the extra
information this provides. This might change when we support tessellation
and geometry shaders.

---

Initially, I had to planned to automatically re-center the viewport when
new content is selected. But it's actually nice to see objects from
a constant position when selecting a new draw command.
This could still be useful as an option or as something we implement
when a draw command from a different render pass is selected? But even
then, not changing the camera pos/rot might be nice.

# Submission synchronization

When application submits to a queue:
- we have to check all pending gui submissions we did and make sure
  the application commands wait for our submissions to complete, if
  it writes any resource we read.
  Except, when the application submission is on same queue as our gui
  submissions, we don't need this, a cmdBarrier (that we insert
  at the end of our gui submission) is enough.
  	- if we don't have timeline semaphores and we already waiting upon
	  the gui submission from another application submission (and they
	  haven't finished yet), things get kinda ugly.
	  Technically, we could chain the second application submission
	  to the first one with a semaphore (and so on in future) but
	  this can create huge chains. Sounds like a nightmare to debug
	  and quite error-prone on the long run.
	  We just bite the bullet and wait for the submission for now.
	  Most workstations have timeline semaphores anyways, making
	  the non-timeline-semaphore codepath *significantly* more complicated
	  seems like a bad idea.
	- a bit tricky: if the gui submission is hooked, we have to know if it will
	  write any of our own copies (of images) that are read by pending
	  draws. If so, we also have to chain them.
	  We cannot simply "use the last available image copy and then block
	  that from begin written" when rendering the gui as that would 
	  require potentially a new hook-record for each submission or 
	  skipping many (potentially all) submission hooks.

When we submit our gui rendering commands:
- check all pending application submissions and make sure our submission
  waits upon all submissions that write application resources we read.
  Except, when they were submitted to same queue. Then the barrier
  we insert at the beginning of gui rendering is enough.
  	- if we don't have timeline semaphores and a previous gui submission
	  already waited upon the application submission (and both still
	  are not finished), we simply wait upon the submission. This shouldn't
	  be a big hit as the gpu is blocked right now anyways by another
	  gui submission.
	- unrelated from all of this we might not want to allow multiple
	  parallel in-flight gui submissions anyways, automatically solving
	  the above problem. But not sure about it, have to see.
	- once again a bit tricky: if any of the pending application
	  submissions is hooked by us and writes an image copy we are reading,
	  we have to chain them via semaphore (if not on same queue)

When application destroys a resource:
- check all pending gui submissions and make sure that all submissions
  using this handle have finished.
  Furthermore, the gui needs to be informed about destruction anyways
  to change its logical state, e.g. might have to unselect the handle.

# On copied image memory

Placing the copied images on the cpu has certain advantages, e.g. for displaying
texel values in the gui. We could also perform more complicated operations
(like computing a histogram) directly on the cpu. 
But vulkan guarantees for cpu-side image support (with linear tiling) is
fairly limited. It might furthermore have a *serious* performance impact.
We therefore try to perform as many operations as possible on the gpu
(e.g. using a compute shader to generate a histogram or finding min/max vals)
and only copy the data to the cpu that is really, absolutely needed there
(e.g. the value of the currently shown texel).

# I/O inspector behavior

Ok, let's figure this out. How should the I/O inspector actually behave?
- What should happen when we continuously update from a command
  buffer/command buffer group? Do we want to try to keep the current
  selection?
  	- YES, this is likely the desired behavior. Everything else makes
	  it fairly useless, possibly deselecting every frame.
- What should happen in the new recording does not have a matching
  I/O slot?
  	- Just deselect. Either show the command or just nothing.
- How to handle the "no matching I/O slot in the command hook"?
  There will be situations where we e.g. say from the inspector "copy
  us descriptor element (4, 1, 5)" but that then does not exist in the
  actual recording.
  Option A: we detect this in the command hook and unset the state
  Option B: we leave the state (allowing the hook to retry in future)
    but simply don't copy/set an appropriate error message somewhere.
  Option C: never match commands for similarity for which that could happen
  	NOTE: i don't think this is a good idea.
	- let's go with option A for now. Option B might have advantages but
	  might result in more flickering. A clean deselect shouldn't lead
	  to problems. Make sure to log something out though, in case this
	  leads to unforeseen unexpected behavior later on.
- What should happen when viewing a CommandRecord that was invalidated,
  possibly referencing invalid data?
  Important to note that the the I/O viewer is not useless in that case
  as this might currently be the latest recording (that just happens to
  have already finished and is invalid) and there may be a new submission
  immediately following.
  	- Imagine a case where all application submissions always already finish
	  and the application destroys the cb/a record-referenced resource 
	  before we draw our gui. In that case we could still provide a fully
	  functional and working I/O viewer.
  	- We likely really have to code fore that. When drawing the inspector,
	  just always check whether resources are still valid.
- Display the I/O inspector when not updating from cb/group?
	- Yes! Often one record is submitted multiple times
	  As long as the record *might* be submitted in future, we have hope
	  that this *might* work. In practice, applications will rarely
	  leave valid records just laying around.
- Display the I/O inspector when a not updating from cb and viewing
  an invalidated record?
  	- No! In that case we know it's never submitted again and we won't
	  be able to make the i/o inspector work. Of course, if we already
	  received a CommandHookState, it can be viewed further.
	  But we won't be able to e.g. change the selected I/O slot, we can't
	  retrieve an update. We could signal that via making the gui elements
	  inactive or just outputting an error message when they are selected.
	  In future, we could introduce a setting/mode in which always *all* I/O
	  resources are fetched into the CommandHookState, allowing this.

# Tracking submissions and groups

We need to support various features for showing submissions in the GUI.

(NOTE: previously, SubmissionBatch was badly named PendingSubmission).

- Get all submissions that happened between two QueuePresent calls.
	- we could implement this by either adding submission IDs or the
	  time of submission to every PendingSubmission.
	- But we furthermore need to keep a PendingSubmission (and the
	  actually submitted records) alive for a while.
	  We could implement that by making PendingSubmission shared and
	  adding a shared (impl: intrusive) pointer to the viewed swapchain
	  on every submission. In QueuePresent then, clear the vector
	  of submissions (impl: we need to buffer frames, release the vector
	  of the last frame so that we always have one valid vector to view).
	  That would even remove the need for submission timestamps/IDs
- how to clean up command groups. Or: what should happen when a command group
  is destroyed even though one of its records is still alive, e.g. being
  viewed in gui?
  	- currently, we crash :(
	- A: don't destroy command groups while they are being viewed
	- B: remember all records associated with command group and unset them.
	- I think A is the sensible variant. We only forget the command groups
	  to not cause a memory leak. Keeping one alive for gui is not a problem.
	  How to implement it:
	  	- I: every CommandRecord holds its group via IntrusivePtr.
		  This creates a cycle since a CommandGroup holds the last associated
		  record via IntrusivePtr as well. So when cleaning up groups
		  (currently in QueueSubmit), we can simply detect that cycle
		  and then delete the group (and its record) if both are not
		  referenced anywhere else.
		- II: alternatively, just take an IntrusivePtr (or, basically just a
		  new `bool CommandGroup::viewedInGui` to just not invalidate the
		  Group if it's needed by the gui. I don't think we ever need
		  the group of a record somewhere else at the moment?
		- I think I is better. As long as a record of a group is alive,
		  we don't even want to destroy that group as chances are high
		  the record is submitted again (if it's being kept alive/valid).
		  Avoids future problems, too, if we one day use CommandRecord::group
		  somewhere else, not aware that it might have been destroyed.
- how closely should CommandHook and I/O inspector be connected?
  Only possible to use inspector when a hook is active?
  	- No, see previous section, last question. When we already have a
	  CommandHookState, the user might want to continue viewing it.
	  The hook might furthermore change in strange ways.
	  We should store I/O inspector state in the cb gui.

NOTE: final implementation turned out a bit different, but is still based
on the same concepts decided here.

# Extension-defensive coding

Ideally, all api call implementations are coded in a way that make them
work without any problems for new extensions. This means

- No object wrapping (can already be disabled via environment variables).
  We could optimize the no-object-wrapping path to make it a better
  alternative though, e.g. use a better unordered_map implementation than stl.
- Dispatch the call down the chain exactly as we got it. Forward
  pNext pointers, don't split up api calls on arrays, don't defer when
  we are calling it etc. Also try not to call any other function down the chain
  in between.

- not sure if this makes sense or not: code defensively in a way that we 
  always handle the case where the application passes us handles we don't
  know? For instance, a future extension could add something like
  `vkCreateGraphicPipelines2` and when we don't intercept it, we don't
  know anything about the used handles. If they are used in `vkCmdBindPipeline`
  and we just expect we know the passed handle, the layer will cause UB/a crash.
  Correctly handling this is (a) a pain in the ass as the implications
  will propagate through almost all of layer code, but also (b) a pain in
  the ass to test, which we would have to do to really make this a useful thing.
  So I don't know if it's a good idea at all.

# On indirect copy

So implementing an "indirect copy" via compute shaders seemed like a bad
idea in the beginning. But seeing that CmdCopyBuffer itself might actually
be implemented via a compute shader on desktop hardware (see e.g. mesas's radv driver), 
this does not seem such a bad idea anymore.
And we won't really get around implementing something like this for raytracing
anyways, can't found any tricks to get around it for 
CmdBuildAccelerationStructuresIndirect since we might not even have any
upper bounds for data that is safe to read (like we do for indirect draw/dispatch).

So, let's come up with a general indirect copy framework.
We will built upon the first concept from node 1749.

First, we always have a compute shader that prepares the command(s).
In this case, it reads the buildRangeInfos and writes the commands into
a buffer (we know its size beforehand). It might need to write out additional
data.
This command will also be able to determine the output size of the copy/copies.
For hooked records where we need indirect copy we will allocate a large
hostVisible buffer with a counter in the beginning into which we will
output the copied data. Might need a second buffer with offset metadata
(allocated at record time, opposed to the other buffer) so we can correctly 
retrieve everything later on.

Second, we simply run CmdDispatchIndirect, possibly multiple times.
For copying indexed data (might happen with indirect draw or accelStruct triangles),
we just have an additional level of indirection.

# Proper buffer_device_address support

How to synchronize when only buffer_device_address is given?

Consider the following scenario:
- An application uses writes a storage buffer by using buffer_device_address
  in async compute
- we retrieve data from the buffer on the gfx queue (e.g. in CommandHook
  or the resource buffer viewer)

Currently, we wouldn't insert any synchronization, resulting in a data
race. Can we even synchronize for this case at all though? We often
have no idea about the buffer_device_address that is used in the end
during submission time. So we can't know which buffer/memory is used
and can't insert synchronization. Retrieving it via commandHook is possible
but (a) might add a lot of overhead if many pipelines use deviceAddress and
(b) we would only know the result after the submission finishes, not
allowing synchronization.

So, we need an alternative approach.
- for reading application buffers during gui rendering (e.g. the buffer resource
  viewer) we simply have to synchronize with *all* submissions that make
  use of buffer_device_address. That hurts but there's no way around it.
  Should only have a real performance hit for async queues.
- commandHook buffer reading is more complicated.
  Doesn't seem too bad at first glance: we will
  only read addresses from state that is currently bound, so just
  relying on the application to manage synchronization is tempting.
  But AFAIK it's allowed to have data or device addresses *accessible* (as long
  as it's not read in the end) in a shader that is currently written 
  somewhere else.
  	- Hm, wait, this is a general problem we have at the moment. We don't/can't
	  synchronize for that at all.
  So we probably have to sync hooked submissions with everything else, in general?
  Might also be painful for performance, especially with async queues. We
  could still expose an option in the settings for that (since it will usually
  work without it I guess; enable it by default though).


How to allow introspection in gui?
How far do we want to follow shader references when copying state in
commandHook? Can we even follow it at all? When we don't know the
used device address at submission time, we can't know if it's valid
when copying - and even if, we can't know its size. We have that information
on the cpu but can't/shouldn't do roundtrips. Having the mapping
from device address to buffer size on the GPU in some form could work (using
then some size retrieval and indirect copy mechanism) but I'd rather not.

Alternatively, we don't follow references at all and only resolve them
when asked for in gui (at that point we can validate whether the address
is valid and how much data we are allowed to read from there). Has,
once again, the fundamental temporal issue though: data may have changed
and might not match the state we are viewing.
	This would be pretty much the same thing we do in the buffer resource
	viewer. Shouldn't be hard to implement/fallback to.

---

Ok, maybe we really want this overly complicated reference-following
mechanism with a gpu (hostVisible) table of all addresses and their sizes.
Can we even really do this? 

If we really follow *all* references and copy all the data, we might end up 
with *huge* copies.
	Could limit the copy size.

The shader might have structures such a linked lists, following so many
reference might add too much overhead
	Could limit the maximum traversal depth.

Also, we know the shader structure at submission time so can schedule/layout
everything, don't need to do that in a shader.

# Revisiting command groups: A better device command overview

For complex applications, the per-frame submission command viewer isn't
easy to use since the submissions done per frame can vary a lot.
Instead, we could leverage our previous concept of command groups to
give a better overview: Basically show all submissions ever done.
But group them so that we can quicky find the submission we are looking for/
quickly get an overview of all the submissions done.
Also group by queue, making use of queues in the UI design is essential
for complex applications with lots of async submissions, lots of work
done over multiple frames.

UI could look like this (using imgui treenodes again):

_ Queue 1: Graphics (family 0)
	- submission group 1
		- command group 1
		- command group 2
		- command group 3
		- command group 4
		- command group 5
	- submission group 2
	...
- Queue 2: Compute (family 1)
	...
- Queue 3: Compute (family 2)
- Queue 4: transfer (family 3)

The groups here are a bit different than our legacy CommandBufferGroup concept.
First, we will filter out "uninteresting" command records that we can't assign
to any meaningful group (could view that via a "rest" entry or separate command mode
for temporary transfer submissions). Any record that uses a pipeline is meaningful,
grouping by used pipeline will already deliver useful results.

We also try to group submissions. Group them by comparing the submitted records.
Not sure if this is useful/meaningful though.

For each submitted command group, we shows metrics detailing how often (and when)
it is submitted. We could mark entries submitted recently (e.g. this or last frame)
green, entries submitted during the last 20 frames yellow and everything red or
something to make this easy to grasp.

We can also use command groups to figure out whether to hook a command.

---

Next iteration on cbGUI:

We need to completely separate the currently shown commands and the
currently shown state:
- we need to store the frame (recordBatch), record and command hierachy
  of the selected command inside the currently shown commands
- we need to store the same for the command associated with the currently shown
  state

They may differ and we need both:
- when freezing commands but not state, the record of the state may be
  up-to-date while the selected command/record/recordBatch is old.
  To correctly show the commands (i.e. show the selected one as selected),
  we need to remember everything of it.
- when freezing state but not commands, the selected command inside the shown
  record/batch may be new while the command associated with the shown state
  might be old. We still need to store the old command from the state to...
  hm, not sure, for what exactly? I guess we want to do matching with
  this command/record combination instead of a newly selected one? But
  it really should not matter. Nah, we should probably even prefer the new
  one since it has less invalidated handles I guess. But might change
  slightly over time, that's the downside.
  
Ok we need it in the following case:
- a command was selected, state is shown, nothing frozen.
  Suddenly the command can't be matched anymore. We still want to update
  the shown commands (e.g. records_), no command is selected in that view.
  Now suddenly, the command reappears, we get a completed hook.
  We want to match its context with the selected batch. So we need that,
  separately from the batch that is currently being shown.

# A full sync prototype

So, as uncovered in the notes about supporting buffer_device_address, we need
more general synchronization. Long story short: We need to synchronize
every access we do on application resources with *everything*. We can't
rely on anything, even in CommandHook submission.

Synchronization with stuff on the same queue is simple, just add a general
barrier (we already do this in most places). Synchronization with
other queues is harder.

With timeline semaphores, we could simply have for each queue:
- a timeline semaphore that was inserted *after* the last submission.
  When hooking a command or submitting the gui cb with access to application
  resources, we simply wait upon these most-recent semaphores
  of all other queues (as long as they still have submissions pending).
- If there is still a gui or hooked submission pending, add a timeline semaphore
  to that. We will wait for it in all future submissions the application makes.

Without timeline semaphores, things look worse:
- we potentially have to insert *a lot* of semaphores later on to other queues, 
  on demand. That is really ugly, causing many vkQueueSubmit calls.

Maybe just don't support full-sync when timeline semaphores aren't supported?
Or at least don't support it for now. The biggest use for full-sync will
be buffer_device_address (ok, granted: and weird applications binding the same
resource from multiple queues at the same time. Sounds bad, but applications are
probably doing it) anyways and drivers will more likely support
timeline semaphores than buffer_device_address I guess. If someone
pops up that really needs the feature (strictly speaking: fix, not feature) for 
some ancient hardware, we can still see how to add it without making the
code 10x more complicated.

---

For gui submission, the only relevant full-sync case is buffer_device_address,
right? When we access an application image during gui rendering, it should
be enough to sync with all submissions that also use that image.
But when we copy application buffer contents, we just can't know which
submissions might use it and must sync with everything.

For commandHook submissions, we might always need full sync. E.g. when
we copy an image, we can't know that another submission is using it at
the same time (even if it was statically used by the shader, it might
not have been dynamically used since the application knows it's not
allowed to do that).

---

While implementing the full-sync concept I noticed several things:
- It's just another code sync code path. We now have 3:
	- binary semaphores
	- timeline semaphores
	- timeline semaphores + full sync
  Where the first two paths should behave the same and the third path
  be somewhat different. This is unfortunate, hard to test.
- At the same time, we kinda want the full sync path for binary
  semaphores as well since it's about correctness, not speed.
- At the same time, I'm not sure we want to enable full sync by default.
  And I'm convinced we want to always allow a non-full-sync path (so
  we shouldn't just always use full sync when using timeline semaphores).
  The reason for that is that we modify the application. And one of the
  goals of the layer was to do that as little as possible. We might need
  to do it in some cases though. But in other cases it might be nicer
  if we didn't do it. Especially, it might be really useful for the user
  to just toggle it at runtime so it might be worth maintaining all these
  paths for now (and maybe even add a binary-semaphore+full-sync path later
  on)

## Questioning full sync

We have full-sync implementation now. During the currently happening
sync/submission rework, I have some new ideas and thoughts.

Do we really ever want full-sync?
In hooked submission, we want to copy data *as the application sees it*.
Adding full sync as soon as we hook a submission somewhat destroys that.
And yes, it might be needed, in that case that the application writes
the data we read from another queue. But then, the application is broken
and we maybe explicitly *want* to show the broken/weird/racy data.
(Only concern: might cause a crash in theory, would be bad).

We only want it for gui submissions I guess.
For images displayed in resource viewer, it doesn't really matter, 
we just sync with the submissions that use it.
For buffers, it harder. buffer_device_address is really a concern.
So maybe only add full sync when reading a buffer with the flag set?

Maybe just leave it up to the application? We kinda need both code paths
anyways for now. And we need sync tracking in any case.

## Memory aliasing

Another concern I just stumbled upon: memory aliasing.
When we sync on a per-resource basis (e.g. we want to show image X in ui
live and therefore just sync with submissions using it somehow), handling
resource aliasing is a pain.
We can track which submissions activate which regions but comparing that
every time just for sync is kinda meh.

How do we handle aliasing in general?
We can't even legally access resources that were activate (implicitly!)
due to another resource in the same memory becoming active.
Maybe handle that via a mechanism similar to 'pendingLayout'?
When an submission makes a resource active (transitions from undefined
layout?) we mark all resources aliasing it as inactive when the
submission is activated?
Complicated, high overhead though.

Maybe just completely ignore aliasing for now?
We might read undefined memory but on desktop it's usually not a big issue.
Have to rethink this when starting to use cows, then it becomes a
bigger issue we have to tackle anyways.
(something along the lines of tracking it as outlined above and resolving
pending cows in 'activateSubmission' in that case)

---

Actually, could just not support cows on aliased resources. Check on 
cow creation time & resource creation time.

---

Quick idea: we already have `forEachOverlappingResourceLocked` in `memory.cpp`.
Activating a resource means transitioning away from undefined layout, right?
That should also be the only way to activate it, anything else is
undefined.
So, every time we get a resource barrier with oldLayout=undefined, we
remember that this resource is activated. On submission activation:

```
for(activation : subm.resourceActivations) {
	invalidate = [&](auto& res) {
		if(res != activationl.res) {
			res.invalidate();
		}
	};

	forEachOverlappingResourceLocked(activation.memAlloc, invalidate);
}
```

But this has some overhead, I'm not sure we want it.
If we can solve cows differently (see above, just not doing cows on aliased
resources), that's probably the better way and enough.

Violating the spec here and actually showing aliased resources that are not
activate in the image/buffer viewer isn't a big deal as long as it does not
cause crashes.

# Relying on timeline semaphores

Sooner or later, we'll likely just require timeline semaphores since that
makes everything so much easier.

- We can get rid of all binary semaphore code paths (that are already
  very inefficient in some places or just don't support some features,
  like full sync)
- We can completely get rid of tracking application submissions via fences
  (don't need a fence pool anymore) and just use the queue submission semaphore
  for that as well.
- Get rid of semaphore resetting concept. Actually, I think we can get
  completely rid of the semaphore pool as well.

The concept of a per-queue semaphore that simply tracks at which submission
it is currently working is so clean and simple, it allows us to get rid
of all the different synchronization mechanisms we needed when there
aren't timeline semaphores.

But to support that, we'd need
- either the translation layer to work well, without large overhead.
  should test that
- or osx, android implementations to support it which might not happen soon :(


# CommandSelection

Idea: factor out matching logic from CommandBufferGui.
We might want to use it outside of CommandBufferGui one day (e.g.
image viewer/shader debugger outside of cb viewer tab) and it makes
CbGui messy as hell atm.

```
struct CommandSelection {
	// Defines from which source the selected state is updated.
	enum class UpdateMode {
		none, // does not update them at all, just select the static record
		commandBuffer, // update them from the commands in the selected command buffer
		any, // update them from any submitted matching record
		swapchain, // update them from the active swapchain
	};

	enum class SelectionType {
		none,
		command,
		submission, // only in swapchain mode
		record, // only in swapchain mode
	};

	// Whether to freeze the selected state. 
	// New completed hooks will still be fetched when something new is 
	// selected but once we have a state we don't gather more and don't
	// update the selection.
	bool freezeState {};

public:
	// Tries to fetch a new completed hook, updating the selection.
	void update();

	// Sets updateMode to 'none'
	// 'cmd' must be empty or a valid hierarchy inside 'record'
	void select(IntrusivePtr<CommandRecord> record, 
		std::vector<const Command*> cmd);

	// Sets updateMode to swapchain
	// 'submission' must be null or inside 'frame'.
	// 'record' must be null or inside 'submission'
	// 'cmd' must be empty or a valid hierarchy inside 'record'
	void select(std::vector<FrameSubmission> frame,
		FrameSubmission* submission,
		IntrusivePtr<CommandRecord> record, 
		std::vector<const Command*> cmd);

	// Sets updateMode to 'commandBuffer'
	// 'cmd' must be empty or a valid hierarchy inside record
	void select(IntrusivePtr<CommandBuffer> cb,
		IntrusivePtr<CommandRecord> record,
		std::vector<const Command*> cmd);

	UpdateMode updateMode() const { return mode_; }
	SelectionType selectionType() const { return selectionType_; }
	IntrusivePtr<CommandHookState> completedHookState() const { return state_; }

	// Returns null when selectType is not 'command'
	span<const Command* const> command() const { return command_; }
	// Returns null when in 'swapchain' mode and selectionType
	// is not 'record' or 'command'
	IntrusivePtr<CommandRecord> record() const { return record_; }
	// Returns null when not in 'swapchain' mode or when selectionType
	// is 'none'
	FrameSubmission* submission() const { return submission_; }
	// Returns null when not in 'swapchain' mode
	span<const FrameSubmission> frame() const { return frame_; }
	// Only for swapchain mode.
	// Returns the present id of the current completed hook state.
	u32 hookStateSwapchainPresent() const { return swapchainPresent_; }

private:
	UpdateMode mode_ {};
	SelectionType selectionType_ {};
	IntrusivePtr<CommandHookState> state_; // the last received state
	CommandDescriptorSnapshot descriptors_; // last snapshotted descriptors

	// The currently selected record.
	// In swapchain mode: part of selectedBatch_
	IntrusivePtr<CommandRecord> record_ {};
	// The selected command (hierarchy) inside selectedRecord_.
	// Might be empty, signalling that no command is secleted.
	// Only valid if selectionType_ == command.
	std::vector<const Command*> command_ {};

	// [swapchain mode] 
	// Potentially old, selected command, in its record and its batch.
	// Needed for matching potential candidates later.
	std::vector<FrameSubmission> frame_;
	// Part of selectedFrame_
	FrameSubmission* submission_ {};
	u32 swapchainPresent_ {}; // present id of last time we got a completed hook

	// [commandBuffer mode]
	IntrusivePtr<CommandBuffer> cb_ {};
};
```

TODO:
- need to give CommandBuffer shared ownership, updating from cb is
  a mess otherwise
  	- this means CommandBuffers can outlive their commandPool, make
	  sure to unset the reference on destruction of the CommandPool

Update: something like this is implemented now.

# Next CommandGroup-like prototype

ideally: be able to show *complete* frame, stable.
Just color in the sections that aren't always submitted or something.

```
namespace summary { 

struct FragmentSection {
	u32 lastSeen {}; // frameID

	IntrusivePtr<CommandRecord> lastRecord;
	Command* first {};
	Command* last {};

	// in case of a parent command, first == last and this
	// contains the child sections
	std::vector<FragmentSection> children;

	// TODO: some extra metadata
	// e.g. for renderpasses, we could store the max number of 
	// draw commands ever seen or something like that.
	// Need some special handling for render passes anyways I guess?
	// But same problematic applies to dispatches, could be highly
	// dynamic as well. hmm.
	// Really go all-in and treat sections of draw commands as fragments
	// as well? But ordering shouldn't matter for them.
	// OTOH for some highly dynamic compute system, order might not
	// matter either. Hard to find good heuristic guessing that though
	// I guess...
	// Maybe store them as fragments but collapse in gui as
	// <206 draw commands> or something?
	// and when expanding them you can toggle whether they should be
	// matched in order or not?
};

struct Record {
	std::vector<FragmentSection> children;
};

struct Submission {
	Queue* queue {};
	std::vector<Record> records;
	u32 lastSeen {}; // frameID
};

struct Frame {
	std::vector<Submission> submissions;
};

} // namespace summary
```

Would be useful to capture commands/submissions that aren't submitted
every frame. Think local shadow updates, transfer queue texture streams etc.
Together with a "capture all" feature (that is similar to what we are already doing
in the shader debugger; don't just capture one resource but all I/O
of a given command) we could then even look into such commands (by
selected them inside the ui even if they are stale and wait for a
submission that gives us a hookState).

With the current design sketch, this will fall apart quickly for
something like draw commands though.

Anyways, we'd have to do *full* record matches (i.e. not just the
sections) at some points, which makes this whole concept probably impossible.

(hm, otoh, we have pretty much just linear complexity if we do this algorithm
the right way. Might be able to make it work).

# Should there be struct PipelineBarrier2Cmd?

Or should it be merged with the original PipelineBarrierCmd
pro for merging:
- less code duplication, might be easier later on
cons for merging:
- introduces complexity
- the data representation isn't what the user might expect
  (or we'd need to write 2 handling methods in most case, e.g. gui, anyways)

Yeah, let's not merge them.
When commands are really different (e.g. different structs/values) and
not just extending each other (e.g. CmdBindVertexBuffers2), use a different
command struct.
If that doesn't cause problems, maybe always do it?
We currently show CmdBindVertexBuffers in the ui even if the 2 version was used.

# Gui rendering without huge critical section

Imagine this situation:
- we are drawing the resource gui, <img> is selected
- <img> gets destroyed in another thread

How should this be handled?
We can keep the vil::Image object alive via IntrusivePtr (e.g. in the 
Gui Draw object) but in this case we also need the handle.
Keeping all image handles alive until their vil representation is destroyed
is problematic. The application might immediate re-use the memory,
making the gui reading (and doing transitions on) the old image UB.

What we could do:
If the detect this case, wait in the destroying thread until gui
rendering is done?
	Done in the sense that we've gotten to the checkpoint where we see
	that the image was destroyed and abort the rendering.

The waiting itself (even if it might take >1ms) isn't a problem, this is 
a very rare case.

Update: yep, solved like this now.

# Old commandSelection matching

Me might need this again if prefix-matching (as we do in CommandHook
to decide whether to hook or not) isn't enough in some case.

```
		// TODO PERF we probably don't want/need all that matching logic
		// here anymore, we match now already when hooking.
		// Maybe just choose the last completed hook here?
		// Or only match (and choose the best candidate) when there are multiple
		// candidates in the last frame?
		for(auto& res : completed) {
			float resMatch = res.match;

			// When we are in swapchain mode, we need the frame associated with
			// this submission. We will then also consider the submission in its
			// context inside the frame.
			std::vector<FrameSubmission> frameSubmissions;
			u32 presentID {};

			FrameMatch batchMatches;
			if(mode_ == UpdateMode::swapchain) {
				// we don't really want to end up here
				dlg_trace("doing re-matching of hooked record");

				[[maybe_unused]] auto selType = selectionType();
				dlg_assert(selType == SelectionType::command ||
					selType == SelectionType::record ||
					selType == SelectionType::submission);

				{
					std::lock_guard lock(dev.mutex);
					auto* frame = frameForSubmissionLocked(res.submissionID);
					if(!frame) {
						continue;
					}

					frameSubmissions = frame->batches;
					presentID = frame->presentID;
				}

				dlg_assert(!frame_.empty());

				// check if [record_ in frame_] contextually
				// matches  [res.record in foundBatches]
				constexpr auto rematch = false;
				if(record_) {
					LinAllocScope localMatchMem(matchAlloc_);
					batchMatches = match(tms, localMatchMem, frame_, frameSubmissions);
					bool recordMatched = false;
					for(auto& batchMatch : batchMatches.matches) {
						if(batchMatch.b->submissionID != res.submissionID) {
							continue;
						}

						for(auto& recMatch : batchMatch.matches) {
							if(recMatch.b != res.record) {
								continue;
							}

							if(recMatch.a == record_.get()) {
								recordMatched = true;
								resMatch *= eval(recMatch.match);
							}

							break;
						}

						break;
					}

					// In this case, the newly hooked potentialy candidate did
					// not match with our previous record in content; we won't use it.
					if(!recordMatched) {
						dlg_info("Hooked record did not match. Total match: {}, match count {}",
							eval(batchMatches.match), batchMatches.matches.size());
						continue;
					}
				}
			}

			if(resMatch > bestMatch) {
				best = &res;
				bestMatch = resMatch;
				bestBatches = frameSubmissions;
				bestPresentID = presentID;
				bestMatchResult = batchMatches;
			}
		}
```

# update_unused_while_pending is evil

Ok so how to support it?
The most general case is updateAfterBind, updateUnusedWhilePending,
partiallyBound *all* being set.
Dynmically used descriptors can be updated any time the submission is
not pending on the device and all bindings that are not dynamically
used can be updated *any* time. That also implies that resources being
bound but that are not dynamically used can be destroyed at any time.

Properly supporting this (for the command hook copy case, i.e. when
a command using such a descriptor set is inspected) is very hard
because we don't know which resources are dynamically used.
So we just always assume the worst:
We get the resource from the ds to read from only at submission time (we already 
do this), this covers the updateAfterBind case.
We use validate ds handles before accessing them (when ds.cpp refBindings 
is false; we do this by not accessing the ds directly but only a cow we made of it).
With this, we already have the worst case partially covered.
The only case missing: we record commands to read from a resource (that is
not dynamically used) but that resource is then destroyed while our
hooked submission is running on the device.

Users could just avoid inspecting resources that are not used dynamically.
And everything will work.
But honestly, we still want to make this work. And currently it's a BAD crash,
memory corruption, whatever. Accessing a destroyed resource. Oh noes.

Could make it work like this, rough sketch:

- Make sure CommandHookRecord knows which handles it uses
- When a buffer/image is destroyed, we first ask CommandHook if it's currently
  in use by any pending CommandHookRecord. 
  If so, wait on the submission before destroying.

That's really costly since we might have a LOT of CommandHookRecords though :/
And in like 99% of the cases of destroyed resources this won't happen.
We could even add a branch to only make this check when the
updateUnusedWhilePending feature is activated (partially bound isn't relevant
since we don't consider static usage either atm, we want to handle
it via the dynamic usage mechanism as well). So, yeah, we can likely
optimize this in some way later on. Maybe store in resources whether they
were ever bound in a updateUnusedWhilePending slot or something.

