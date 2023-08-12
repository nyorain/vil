fuen: how to read out indirect-indexed-draw

(moved here from node 1749)

# TransformIndirect
First, run compute shader that reads the indirect command.
It transforms it into a VkDispatchIndirectCommand.
The total number of invocations are the number of drawn indices (indexCount).

# ProcessIndices
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
	DispatchIndexedIndirectCommand dstCmd;
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
			ownMin = min(ownMin, index);
			ownMax = max(ownMax, index1);
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
	// potentially accounts for bytes, strides, etc
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

Hm, nope, we probably don't want to do everything at once.
Taking inspiration from the radv meta shaders, we want to keep things simple
and just copy one word at a time, makes more sense.
Just dispatch for each buffer to be copied etc.

---

We still might have the case where the draw command only draws a couple of
vertices but the indices are *all over* a huge buffer.
In that case would copy a lot where we don't need to.
If we, instead, evaluated the indices and only copied that, things would work better.
But that is bad in worse normal cases.
We could, later on, add an optimization that chooses the mode dynamically,
in the WriteVertexCmd shader. We write two indirect commands there and store
the used mode in some meta dst buffer.

---

Let's do that now! The system is already getting complicated

WriteVertexCmd:

```
layout(set = 0, binding = 0) buffer InData {
	uint indexCount;
	uint vertexSize; // in uints

	uint minIndex;
	uint maxIndex;
};

layout(set = 0, binding = 0) buffer OutData {
	DispatchIndirectCommand dstCmd;
	uint dstType;
}

void main() {
	if(maxIndex - minIndex < indexCount) {
		// in this case, we copy the vertices just as they are in the vertex buffer
		dstType = copyVertices;
		dstCmd.x = maxIndex - minIndex;
	} else {
		// In this case, we copy the vertices as addressed by index buffer
		// We still copy the index buffer just to show its data in the UI
		dstType = resolveVertices;
		dstCmd.x = indexCount;
	}
}
```

CopyVertexCmd:

```
layout(set = 0, binding = 0) readonly buffer Indices {
	uint copyType;
};

layout(set = 0, binding = 1) buffer InData {
	uint indexSize;
	uint indexCount;
	uint vertexSize; // in uints
};

layout(set = 0, binding = 2) readonly buffer InVertices {
	uint data[];
} src;

layout(set = 0, binding = 3) readonly buffer OutVertices {
	uint data[];
} dst;

void main() {
	uint offset = gl_GlobalInvocationID.x;

	if(copyType == copyVertices) {
		for(auto i = 0u; i < vertexSize; ++i) {
			dst.data[offset * vertexSize + i] = src.data[offset * vertexSize + i];
		}
	} else {
		uint index;
		if(indexType == 2) {
			index = srcIndices[offset / 2];
			if(indexID % 2 == 1) {
				index = index & 0xFFFFu;
			} else {
				index = index >> 16u;
			}
		} else {
			index = srcIndices[offset];
		}

		for(auto i = 0u; i < vertexSize; ++i) {
			dst.data[offset * vertexSize + i] =
				src.data[index * vertexSize + i];
		}
	}
}
```

CopyVertexCmd8:

```
layout(set = 0, binding = 0) readonly buffer Indices {
	uint copyType;
};

layout(set = 0, binding = 1) buffer InData {
	uint indexSize;
	uint indexCount;
};

layout(set = 0, binding = 2) readonly buffer InVertices {
	u8 data[];
} src;

layout(set = 0, binding = 3) readonly buffer OutVertices {
	u8 data[];
} dst;

pcr {
	uint vertexSize; // in bytes
}

void main() {
	uint offset = gl_GlobalInvocationID.x;

	// NOTE: glsl intrinsic for spirv OpCopyMemorySized would be nice,
	// might be more efficient

	if(copyType == copyVertices) {
		for(auto i = 0u; i < vertexSize; ++i) {
			dst.data[offset * vertexSize + i] = src.data[offset * vertexSize + i];
		}
	} else {
		uint index;
		if(indexType == 2) {
			index = srcIndices[offset / 2];
			if(indexID % 2 == 1) {
				index = index & 0xFFFFu;
			} else {
				index = index >> 16u;
			}
		} else {
			index = srcIndices[offset];
		}

		for(auto i = 0u; i < vertexSize; ++i) {
			dst.data[offset * vertexSize + i] =
				src.data[index * vertexSize + i];
		}
	}
}
```

- We only use the 8-bit variant when needed, known at record time
	- on devices not supporting it, we just can't capture mesh data
	  in that case I guess

Xfb improvements: For DrawIndirect(Count), split up the individual indirects
draws. Also use the size hint.
- in hookRecordDst, when copyXfb is true, don't just call dispatchRecord
  when it's a drawIndirect(Count). But instead split it up.
  First all previous commands, then the target command, then all commands
  after this.
  	- not so easy for DrawIndirectCount. We don't know at record time
	  how many we have in the first place.
	  So just dispatch compute shader that splits them up into multiple
	  count buffers, writes 0 for target and after-commands if needed.
	  New indirect-cmd-offset calculation can be done at record time.

===

For all these shaders, we now need to create DescriptorSets in a hooked record.
Livetime-wise not a problem, just kept there until it's destroyed.
But how to manage DescriptorPool?
Best approach is probably a growing list of pools in Hook.

