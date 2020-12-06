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

In the end, we are trying to go for full shared ownership for all handles
(it still makes sense for information that are truly shared and not owned
by a specific handle/might outlive it like spirv shader module or renderpass
data).
