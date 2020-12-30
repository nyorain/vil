# Performance considerations using the layer

The layer will track *all* state, therefore result in a significant memory
and also a potentially significant cpu overhead. This is acceptable
and optimizing every drop of performance and memory consumption out of the
layer isn't practical; being able to quickly extend it and add support for
new features and extensions is more important.

But there are a couple of goals:
- Programs that are usable without layer should stay playable with layer.
  This means increasing frame timings by something like 5% is acceptable,
  while 50%, for instance, is not. For initialization/creation times, a
  more significant overhead is acceptable.
- The memory consumption should stay in comparable manageable sizes.
  Adding up to something like 10% memory consumption to a program is to
  be expected, values above 50% could make programs unusable and should
  be considered issues.

In practice, the worst performance impact possible comes likely from
the serialization of many commands done in the layer: per-resource
synchronization is hard/not possible in general and so synchronization
is mostly done on a per-device level (that also involves the gui, while
it is being drawn no resources can be created or destroyed). 
While the most expensive things (mainly command buffer recording) can be done 
inside the layer mostly without requiring a mutex lock, most operations
require a lock at some point. Programs that use vulkan heavily from multiple
threads might be slowed down significantly.
