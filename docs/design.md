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

A known issue of the fuencaliente design: host allocators might not
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
