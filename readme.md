# Fuencaliente

Relatively small vulkan in-app debugging layer that is able to provide
as much introspection as possible via a debug window.

Work in progress, this is pretty much just a skeleton right now.

Features:
- showing all created resources, their usage, content and connection
	- can be identified via debug names
	- visualize content of images and see buffer content
	- take temporary snapshots of resources; "freeze" them to inspection
- inspect recorded and executing command buffers with gpu timings
	- quickly inspect pipeline state
- get overall statistics such as number of allocated resources,
  total memory allocations

Intended much more as a live debugging tool than a profiling tool.
Does not try to replace capture-based debugging tools like renderdoc,
this layer will probably never provide things such as:

- draw-by-draw debugging of render targets
- shader debugging
- pixel histories

But, unlike capture-based tools, this allows a **live** introspection.
This can be useful to debug temporal issues or as a first-try
debugging trying to figure out roughly where a problem is, without
waiting for a capture to load. It also is useful as a general-purpose
introspection tool, capture-based tools are usually optimized to only show
resources important for their capture, leaving out the rest.
It also allows to easily debug compute-only vulkan applications that
don't have swapchain (renderdoc can do that but requires the application
to explicitly call renderdoc api).
