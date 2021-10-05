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
