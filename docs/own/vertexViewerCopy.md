how to read out indirect-indexed-draw

- TransformIndirect
First, run compute shader that reads the indirect command.
It transforms it into a VkDispatchIndirectCommand.
The total number of invocations are the number of drawn indices (indexCount).

- ProcessIndices
Then, a second compute shader is dispatched indirectly, with the generated
indirect command. It reads the indices from the buffer and copies them into
a destination buffer. Per index, it also reads the vertices from their
buffers and writes them to destination buffers.
???

profit!

---

ok it's not that simple.
- either, really only the vertices in order, into append buffer
  when an application has a small number of vertices and a high number
  of indices, this might explode
- or, copy the rendered indices and the *vertex range* accessed by them.
  In the index copy compute shader, just generate atomicMax of indices
  and then run a second compute shader, copying that vertex range.
  When an aplication has *huge* vertex buffers and a single draw
  call has indices pointing to vastly different sections, this might explode.
  I guess this usually does not happen though.
  **let's go with this for now**

simpler approaches
- Just use the indirect command fetched the last time.
  This means we have to wait 2 submissions until actually getting vertex data :(
  And it breaks in cases where e.g. offsets are completely dynamic,
  e.g. sub-allocated just-in-time by application.
  Also, what means *last time*? would build things inside the command hook
  itself upon our command matching heuristic (for applications that only
  have single-usage command buffers), i don't like this at all!

important optimization. But this is for later on, I feel like we shouldn't
just build everything upon this.
- check whether a buffer can actually be changed between two copies.
  mappable? transfer command executed? shader using buffer as writable SSBO?
  if none of this is the case, we can assume it hasn't changed I guess (well,
  might have to think about aliasing, the guarantees given by vulkan).
  If it hasn't changed, don't copy it again.

---

Transform indirect:

```
layout(set = 0, binding = 0) buffer SrcCmd {
	DrawIndexedIndirectCommand srcCmd;
};

layout(set = 0, binding = 1) buffer DstCmd {
	DispatchIndirectCommand dstCmd;
};

void main() {
	dstCmd.x = srcCmd.indexCount;
	dstCmd.y = 0;
	dstCmd.z = 0;
}
```

nvm, we can just use a one-word buffer copy.

ProcessIndices:

```
layout(set = 0, binding = 0) readonly buffer Indices {
	uint srcIndices[];
};

layout(set = 0, binding = 1) buffer OutData {
	uint minIndex;
	uint maxIndex;
	uint dstIndices[];
};

layout(push_constant) uniform PCR {
	uint indexType;
	uint indexCount;
}

void main() {
	uint offset = gl_GlobalInvocationID.x * 8;

	uint ownMax = 0u;
	uint ownMin = 0xFFFFFFFFu;

	for(uint i = 0u; i < 8 && offset + i < indexCount; ++i) {
		uint index = srcIndices[offset + i];
		if(indexType == 2) {
			uint index0 = index & 0xFFFFu;
			uint index1 = index >> 16u;
			ownMax = max(ownMax, index0);
			ownMax = max(ownMax, index1);

			ownMin = min(ownMin, index0);
			ownMin = min(ownMin, index1);
		} else {
			atomicMax(ownMax, index);
			atomicMin(ownMin, index);
		}

		dstIndices[offset + i] = index;
	}

	atomicMax(maxIndex, ownMax);
	atomicMin(minIndex, ownMin);
}
```

WriteVertexCmd:

```
layout(set = 0, binding = 0) buffer InData {
	uint minIndex;
	uint maxIndex;
};

layout(set = 0, binding = 0) buffer OutData {
	DispatchIndirectCommand dstCmd;
}

void main() {
	dstCmd.x = maxIndex - minIndex;
}
```

CopyVertexCmd:

```
layout(set = 0, binding = 0) readonly buffer InBuffers {
	uint data[];
} src[8];

layout(set = 0, binding = 1) readonly buffer OutBuffers {
	uint data[];
} dst[8];

void main() {
	uint offset = gl_GlobalInvocationID.x * 8;

	for(uint i = 0u; i < 8; ++i) {
		uint count = src[i].data.length();
		for(uint o = 0u; o < 8 && offset + o < count; ++o) {
			dst[i].data[o] = src[i].data[o];
		}
	}
}
```

something like that?

important: even if we have to copy a large vertex/index buffer range,
	we don't want to make a cpu copy of all of that. Only a small
	range/what we currently need in the table!
	When the hook state is old, not used in a record anymore (e.g. it was
	frozen; we should be able to make sure frozen states are no longer
	used) we can dynamically update it still, we have the data in a VkBuffer
	after all.
