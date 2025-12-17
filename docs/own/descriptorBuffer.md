# Support for VK_EXT_descriptor_buffer

Main challenge: How do we get the descriptors from a record?
Cannot do so at submission time anymore.

Let's assume we hook a record for which we want to introspect a
specific descriptor binding:

- in the hooked record, we can access and save the descriptor data blob
- but how to resolve that data blob and copy the data??
	- worst case: mutable descriptors. We can't even know the type of
	  the descriptor

Just don't allow descriptor introspection for now, there is no easy and
fast solution that works in all cases.

## Best we can do approach for later

How far do we get with non mutable descriptors?
Looking stuff up on the gpu will likely not work properly.
	Complicated hashmap data structres on the GPU?
		+ device generated commands?? lol

How far could we get with the copy_indirect extension?
	hm, not so far.

Idea! We don't need to know the handle on the gpu.
We could just access the descriptor! Just copy via shader.
Still has some limitations (acceleration structures?) but it's a start.

How could we handle acceleration structures?
Could make sure state isn't overwritten by submission and store some
serial number to identify it later on.
	-> for later

#### Can we handle mutable descriptors with this?

Can we somehow encode the type into the descriptor? i.e. change
	the return value of GetDescriptorEXT?
		while the descriptor still works? meh, likely not
Would a lookupmap even work? could different descriptor types end
	up with the same memory? Unlikely but possible I guess.

sad :(

---

Return our own handles from GetDescriptorEXT and let a compute
shader run before each draw/dispatch that fixes everything up? :D
Terrible idea.

---

Maybe we can implement heuristics for the type?
e.g. looking at the different descriptor sizes

have a look at how the shader accesses the descriptor?
	might still be only bound to single binding, not aliased?

that together with hash map on gpu (that should usually work)
might be enough in like 99% of the cases.

### How to indirectly copy

Indirect dispatch. But how to know the size?
	For images and storage buffers, we can query it!
	Uniform buffers? meh
	Just copy a couple of bytes and figure it out later on the CPU? :D
Inspect shader that uses it?
	if the slot is bound as a uniform buffer, just use its size.
	if it has multiple uniform buffers alised at the binding,
	just choose the smallest? edge case anyways

for image/storage buffer: how to create/allocate dst memory?
	feedback loop about size like we already do for transform feedback etc
	at some point we can think about a dynamic allocator on the gpu
	(requiring us just to reserve a buffer range instead of creating
	a resource)
