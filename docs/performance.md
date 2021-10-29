# Performance considerations using the layer

The layer will track *all* state, therefore result in a significant memory
and also a potentially significant cpu overhead. This is acceptable
and optimizing every drop of performance and memory consumption out of the
layer isn't practical; being able to quickly extend it and add support for
new features and extensions is more important.

But there are a couple of goals:
- Programs that are usable without layer should stay usable with layer.
  This means increasing frame timings by something like 10% is acceptable,
  while >100%, for instance, is not. For initialization/creation times, a
  more significant overhead is acceptable.
- The memory consumption should stay in comparable manageable sizes.
  Adding up to something like 10% memory consumption to a program is to
  be expected, values above 50% could make programs unusable and should
  be considered issues.

In practice, the worst performance impact possible comes likely from
the central mutex locking of many commands done in the layer: per-resource
synchronization is hard/not possible in general and so synchronization
is mostly done on a per-device level (that also involves the gui, while
it is being drawn no resources can be created or destroyed). 
While the most expensive things (mainly command buffer recording) can be done 
inside the layer mostly without requiring a mutex lock, most operations
require a lock at some point. Programs that use vulkan heavily from multiple
threads might be slowed down significantly.

## General notes

(We should probably migrate this to an extra page as it's advice for
 the user while the other sections are dev-focused).

- Enabled transform feedback (via VIL_TRANSFORM_FEEDBACK) can cause
  pipeline cache misses causing a *massive* increase of pipeline
  creation time that may cause slowdowns.

## Layer Profiling

To profile critical performance and memory overhead, there are mechanisms 
to profile the layer. Currently, we use [tracy](https://github.com/wolfpld/tracy/)
as profiling tool. Look at the manual in pdf form attached to their github
releases for details. 
We use on-demand profiling by default. 
In games, there may still be *a lot* of zones, that's why we have a
`VIL_EXTENSIVE_ZONES` option macro in `util/profiling.hpp`. Functions
that are called very often but could still be interesting for profiling
will only emit profiling zones when this macro is defined (for instance:
every draw/dispatch command or internal record invalidation).

We also profile the times we spent waiting for locked mutexes (since this
can be a major problem in multi-threaded applications). The tracy visualizer
may be overwhelmed with our amount of locks though, causing it to become
unusably slow. Just disable visualization of the locks via the options.

The profiler is proven and maintained, new features should always check
their overhead in real-world applications.
In may 2021, for instance, this was used to identify the old descriptor
set tracking system as a major bottleneck for heavily multi-threaded
applications (tested for instance with dota). Profiling runs were
also the ground on which the (optional) support for handle wrapping
was added - not the hash map lookup itself that we have to do without wrapping
was a problem but the fact that we have to lock a (shared) mutex to access
that map in the first place. If another thread is currently performing an
expensive operation, holding the thread for a long time, this can cause
significant slowdown for trivial operation, e.g. every single draw call.
With handle wrapping we can perform many fast-path operations (such as
vkCmdDraw, vkCmdDispatch, vkCmdBindDescriptorSet, vkCmdPushConstants) without
locking a single mutex.

Todo
- allow to enable/disable it (as well as some more detailed options) via meson_options
- better memory tracking, we just track a small number of places at the moment
  that were suspects for leaks or significant overhead in the past.

Notes/observations:
- Copying large amounts of data on the GPU is generally not a problem.
  Even capturing many MBs of data (e.g. with transform feedback or buffer/image
  inspection) usually runs smoothly. It breaks down only when capturing
  like houndred MBs in every frame (could only achieve this with transform
  feedback where a single draw command draws the whole scene so far).
  On the CPU, we are much more limited and should *never* variable sized
  application data when we can get around it. Copying even 4MB per
  frame via memcpy can cause significant problems. Just use the mapped
  GPU buffers directly.
