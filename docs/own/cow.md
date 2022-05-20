Copy on write optimization for CommandHook
==========================================

In CommandHook, we sometimes have to copy *huge* buffers and images.
But most of the time, that copy isn't actually needed since the application
doesn't modify the data anyways. So instead of always copying, we can
just implement a copy-on-write mechanism where possible.

Keep in mind:
- In some cases, we can't detect writes (mapped buffers/images,
  buffers with a DeviceAddress etc)
- We have to implemenet CoW conservatively, i.e. have to copy in all
  cases before the application *could* write something (e.g. storage buffer).

```
struct CowImage {
	std::optional<CopiedImage> own;

	Image* source;
	VkImageSubresourceRange range;
};

struct CowBuffer {
	std::optional<OwnBuffer> own;

	Buffer* source;
	VkDeviceSize offset;
	VkDeviceSize size;
};

// In image (buffer analogously)
{
	....
	std::vector<CowImage*> cows;
};

// In CommandHookState, e.g. dsCopy
{
	std::unique_ptr<CowImage> image;
}
```

When a submission would modify a resource that has connected cows (or when
such a resource is destroyed), we simply
- first submit the copy operation
- disconnect the cow from the resource 'cows' vector and unset the source in it

We need special handling for resource destruction: We'd have to make sure to
keep the resource alive until our copy submission finishes.

### Timing & sync

What if the gui has a pending submission using an application resource
for drawing (cow saved us a copy) but then the application modifies
it:

- if it modifies it directly (e.g. MapMemory) we'd have to block in the
  call until our submission returns which is problematic. Maybe don't
  use the cow mechanism for mappable resources for now
- if it modifies it via a submission, we can simply chain that submission
  to our gui submission (via timeline semaphore).

---

What if the gui is *recording* it's submission while this happens?
We might have a race here.

Solution: We somehow have to signal *in the cow object* whether the
original object is *scheduled for use*. Just a semaphore isn't enough
tho since we might not have it/it might be signaled.
A timeline semaphore would probably solve it but create an ugly
dependency where we have to make an application submission wait
on our *cpu-side* gui rendering to finish by waiting on a semaphore/timepoint
with an associated submission that is still being built.

---

The operation resolving the cow needs to be synced with *all* operations
the application does involving the resource.
Actually, it's only that strict for images since we might have to
transition them.

The transform feedback problem
==============================

This won't solve our transform feedback problem, where we sometimes have
waaay too much data as well (causing significant slowdowns even on
high end gpus). The solution for that problem is paging. The problem
is that the vulkan xfb api does not allow to specify a capture
offset. So we'd have to manually split a too-large draw call into multiple
smaller ones with doesn't sound trivial. Especially for indirect multi draw.
Maybe just make split up multi draw into the individual draw calls and
implement paging on that base? We can then later on still investigate how to
split up single draw calls, we then have to do both anyways.

# Figuring out when a buffer/image is (potentially) written

We only want to add Cows on large objects anyways (or maybe when there
are many objects e.g. descriptorSet with >100 images). But for those
we need to figure out when they are written to correctly resolve the cow.
Multiple problems:

- For image variables in shaders, we know whether they are readonly or not,
  for buffers we don't. To make this work well with large storage buffers
  (think e.g. vertex/index buffers for raytracing) we just need to do
  our own shader analysis. Basically, just check each OpStore/OpImageStore
  instruction or whatever else may write to image/buffer from shader.
- When to check?
  During recording, we know which bindings are potentially written to.
  But that might bring quite some overhead for *all* records.
  Alternatively, we could only check during hook/submission time, in most
  cases there won't be any active cow and we can skip everything.
  Probably better to not do it during recording already.
- store a global list of all cow objects for this?

pseudo code in submission logic:

checkCow(submission)
	if there are any cows
		for usedHandle : submission.usedHandles
			if usedHandle.handle is descriptorSet
				for command : usedHandle.commands
					store bindings potentially written by pipeline
				for binding : bindings
					if binding is potentially written by any pipeline
						resolveCows(binding)
			else if usedHandle.handle is image
				if usedHandle.imageWritten
					resolveCows(usedHandle)
			else if usedHandle.handle is buffer
				if usedHandle.bufferWritten
					resolveCows(usedHandle)

The most costly one will be the descriptorSets.
But for most sets we can probably early-out, add fast path in
layout for sets that don't have any writable bindings to begin with?
					
	
