## Design

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

A known issue of the vlid design: host allocators might not
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
as well/build it themselves from master).

# Window input hooking

This one shouldn't even be officially supported, it's such a hack. Anyways,
useful as a super-quick dropin. When applications don't explicitly use the
fuen api to pass input to the layer (for gui stuff) we can atempt to hijack
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

# On the Vertex Viewer

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
	  
