/// For CommandBuffers, commands are allocated in per-CommandBuffer
/// storage to avoid the huge memory allocation over head per command.
// template<typename T>
// struct DestructorCaller {
// 	void operator()(T* ptr) const noexcept {
// 		ptr->~T();
// 	}
// };
//
// using CommandPtr = std::unique_ptr<Command, DestructorCaller<Command>>;

---

struct BoundImage {
	std::atomic<BoundImage*> nextImage; // linked list for same ds
	std::atomic<BoundImage*> nextBind; // linked list for same image

	Image* image;
	DescriptorSet* ds;
	unsigned binding;
	unsigned elem;
};

// nah, better:
struct Descriptor::Binding {
	std::atomic<Descriptor::Binding*> next1; // images, buffers, bufferViews
	std::atomic<Descriptor::Binding*> next2; // samplers

	bool valid {};

	union {
		ImageInfo imageInfo;
		BufferInfo bufferInfo;
		BufferView* bufferView;
	};

	DescriptorSet* ds;
	unsigned binding;
	unsigned elem;
};

// binding an image view: simply insert into image.descriptors linked list
// deleting image view: iterate through linked list, directly invalidating each binding
// invalidating descriptor: unlink from (non-null) lists, set valid to false
// deleting descriptor: simply invalidate all bindings

---

command buffer group concept:

```cpp
struct CommandBufferSection {
	std::string name; // command/label name information

	// statistics about number of command types
	u32 transferCommands {};
	u32 drawCommands {};
	u32 computeCommands {};
	u32 syncCommands {};
	u32 queryCommands {};

	std::vector<CommandBufferSection> children;
};

struct SubmissionDescription {
	Queue* queue;
	CommandBufferSection sections; // nested tree

	CommandPool* pool; // not sure if useful
	std::vector<VkPipelineStageFlags> waits; // not sure if useful
	// something regarding the other cbs this one was grouped with?
	// something on whether a fence was used?
};

// in device.hpp
// maybe per-queue?
std::vector<std::unique_ptr<SubmissionDescription>> submissionGroups;

// Per cb in Submission?
SubmissionDescription* submissionGroup {};

// in submit:
for(cb : cbs) {
	auto desc = getSubmissionDesc(queue, cb);
	// this matching has a low threshold, when command buffers have even
	// roughly the same structure and are submitted to the same queue,
	// they are considered to be in one group. (maybe allow to even completely
	// disregard structure, showing all commands submitted to one queue?)
	// NOTE: with the notes below, no we should have a decently-high treshold
	// by default to avoid flickering from unrelated cbs. Showing "all commands
	// submitted to a queue" isn't something that's as useful/well-defined
	// in the gui. Could be re-investigated later on.
	auto sg = find(queue.submissionsGroups, desc);
	if(!sg) {
		sg = queue.submissionsGroups.emplace_back(make_unique(desc)).get();
	}

	subm.cb.sg = sg;
}

```
in command buffer gui:
- allow to select SubmissionDescription instead of just a cb.
  then, in each frame, we find all (or just one? or just the pending?) cbs 
  matching that description and display that
  __this is where is gets kinda sketchy__
  	- maybe assume for now there is at most one cb of a group submitted
      at a time. If none is submitted, use the state of the last submitted
	  one (can be kept alive by command viewer)
- we can use our CommandDescription concept as usual, it's not tied
  to a single command buffer by design.

We should already tie this in with the reset-stabilization concept I think.
We never want this command buffer state flickering in the UI. That means
showing the previous command buffer state.
For a command buffer group, we should then simply display the last known
command buffer state (even if command buffer was reset/destroyed) of
that group (or maybe all commands from this group submitted last frame?
but we don't wanna rely on this whole frame concept too much; one of
our strengths is that we can more easily make cross-frame submissions
and stuff visible).

- heuristics for finding the "primary", most interesting submission
	- the one chained via semaphore to present?
	  the one with the render pass?

---

so, sooner or later we might need this command buffer state concept

```cpp
struct CommandBufferRecord {
	Command* commands {};
	CommandBuffer* cb {}; // might be null when cb is destroyed
	CommandPool* pool {}; // set to null when pool is destroyed

	CommandMap<VkImage, UsedImage> images;
	CommandMap<VkBuffer, UsedBuffer> buffers;
	CommandMap<std::uint64_t, UsedHandle> handles;

	std::vector<DeviceHandle*> destroyed;
	CommandVector<IntrusivePtr<PipelineLayout>> pipeLayouts;

	// We own those mem blocks, could even own them past command pool destruction
	CommandPool::MemBlock* memBlocks {};
};

struct CommandBufferGroup {
	CommandBufferRecord* lastRecord; // shared ptr? intrusive ptr? no idea about ownership yet

	std::vector<CommandBuffer*> cbs;

	Queue* queue {};
	CommandBufferSection sections; // nested tree
};
```

---


struct BufferContentHook : CommandHook {
	std::vector<std::byte> lastData {};
	std::vector<TimeCommandHookSubmission*> pending;

	std::vector<CommandDesc> before {};
	VkBuffer src;
	VkDeviceSize srcOffset;
	VkDeviceSize srcSize;

	u32 counter {0};
	std::vector<BufferContentHookRecord*> records {};
};

struct BufferContentHookRecord : TimeCommandHookRecord {
	VkCommandBuffer cb {};

	// todo replace with allocation from pool
	VkDeviceMemory devMem {};
	VkBuffer buffer {};

	~BufferContentHookRecord();
	void finish() override;
};

struct BufferContentHookSubmission : TimeCommandHookSubmission {

};

----

const auto stages = {
	VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
	VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
	VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
	VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT,
	VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT,
	VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT,
	VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
	VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
	VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	VK_PIPELINE_STAGE_TRANSFER_BIT,
	VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
	VK_PIPELINE_STAGE_HOST_BIT,
	VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
	VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	// VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
	// VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT,
	// VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
	// VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
	// VK_PIPELINE_STAGE_SHADING_RATE_IMAGE_BIT_NV,
	// VK_PIPELINE_STAGE_TASK_SHADER_BIT_NV,
	// VK_PIPELINE_STAGE_MESH_SHADER_BIT_NV,
	// VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT,
	// VK_PIPELINE_STAGE_COMMAND_PREPROCESS_BIT_NV,
	// VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_NV,
	// VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_NV,
	// VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
};


---

VkRenderPass recreate(const RenderPassDesc& desc) {
	Device dev; // todo
	bool have2 = true;
	VkRenderPass rp;

	if(have2) {
		VkRenderPassCreateInfo2 rpi {};
		rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
		rpi.pNext = desc.pNext;
		rpi.flags = desc.flags;
		rpi.subpassCount = desc.subpasses.size();
		rpi.pSubpasses = desc.subpasses.data();
		rpi.attachmentCount = desc.attachments.size();
		rpi.pAttachments = desc.attachments.data();
		rpi.dependencyCount = desc.dependencies.size();
		rpi.pDependencies = desc.dependencies.data();

		VK_CHECK(dev.dispatch.CreateRenderPass2(dev.handle, &rpi, nullptr, &rp));
	} else {
		std::vector<VkAttachmentDescription> attachments;
		downgrade(attachments, span<const VkAttachmentDescription2>(desc.attachments));

		std::vector<VkSubpassDependency> dependencies;
		downgrade(dependencies, span<const VkSubpassDependency2>(desc.dependencies));

		std::vector<VkSubpassDescription> subpasses;
		std::vector<std::vector<VkAttachmentReference>> references;

		auto downgradeAttRefs = [&](const VkAttachmentReference2* refs, std::size_t count) {
			if(count == 0) {
				return u32(0);
			}

			auto off = references.back().size();
			for(auto i = 0u; i < count; ++i) {
				auto& attSrc = refs[i];
				auto& attDst = references.back().emplace_back();
				attDst.attachment = attSrc.attachment;
				attDst.layout = attSrc.layout;
			}

			return u32(off);
		};

		for(auto& src : desc.subpasses) {
			auto& dst = subpasses.emplace_back();
			dst = {};
			dst.flags = src.flags;
			dst.colorAttachmentCount = src.colorAttachmentCount;
			dst.inputAttachmentCount = src.colorAttachmentCount;

			dst.preserveAttachmentCount = src.preserveAttachmentCount;
			dst.pPreserveAttachments = src.pPreserveAttachments;

			auto& atts = references.emplace_back();
			auto colorOff = downgradeAttRefs(src.pColorAttachments, src.colorAttachmentCount);
			auto depthOff = downgradeAttRefs(src.pDepthStencilAttachment, src.pDepthStencilAttachment ? 1 : 0);
			auto inputOff = downgradeAttRefs(src.pInputAttachments, src.inputAttachmentCount);

			if(src.pResolveAttachments) {
				auto resolveOff = downgradeAttRefs(src.pResolveAttachments, src.colorAttachmentCount);
				dst.pResolveAttachments = &atts[resolveOff];
			}

			dst.pColorAttachments = &atts[colorOff];
			dst.pInputAttachments = &atts[inputOff];
			if(src.pDepthStencilAttachment) {
				dst.pDepthStencilAttachment = &atts[depthOff];
			}
		}

		VkRenderPassCreateInfo rpi {};
		rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		rpi.pNext = desc.pNext;
		rpi.flags = desc.flags;
		rpi.subpassCount = subpasses.size();
		rpi.pSubpasses = subpasses.data();
		rpi.attachmentCount = attachments.size();
		rpi.pAttachments = attachments.data();
		rpi.dependencyCount = dependencies.size();
		rpi.pDependencies = dependencies.data();

		VK_CHECK(dev.dispatch.CreateRenderPass(dev.handle, &rpi, nullptr, &rp));
	}

	return rp;
}

---

```
	/*
	// find all resolve attachments and the subpasses that resolve content
	// into them (as well as the resolve src attachment ids).
	std::vector<u32> srcResolveOps; // maps src attachmentsID -> subpassID
	std::vector<u32> dstResolveOps; // maps dst attachmentsID -> subpassID
	srcResolveOps.resize(desc.attachments.size(), u32(-1));
	dstResolveOps.resize(desc.attachments.size(), u32(-1));

	for(auto s = 0u; s < desc.subpasses.size(); ++s) {
		auto& subp = desc.subpasses[s];
		if(!subp.pResolveAttachments) {
			continue;
		}

		for(auto i = 0u; i < subp.colorAttachmentCount; ++i) {
			if(subp.pResolveAttachments[i].attachment == VK_ATTACHMENT_UNUSED) {
				continue;
			}

			auto srcID = subp.pColorAttachments[i].attachment;
			auto dstID = subp.pResolveAttachments[i].attachment;
			srcResolveOps[srcID] = std::min(srcResolveOps[srcID], s);
			dstResolveOps[dstID] = std::min(dstResolveOps[dstID], s);
		}
	}

	// if one of the attachments resolved into is potentially
	// read before or written to afterwards (also counts for src for writing),
	// splitting the renderpass does not work.
	for(auto s = 0u; s < desc.subpasses.size(); ++s) {
		auto& subp = desc.subpasses[s];
		for(auto a = 0u; a < subp.colorAttachmentCount; ++a) {
			auto attID = subp.pColorAttachments[a].attachment;

			// when the attachment is written in this subpass s but
			// was read or written before in a resolve operation, then splitting
			// the renderpass (at a subpass >= s) will potentially result
			// in different contents in this attachment.
			if(srcResolveOps[attID] < s || dstResolveOps[attID] < s) {
				return false;
			}
		}
	}
	*/
```


For Buffer layout detection:

```
struct BufferLayoutInfo {
	// Where the information comes from
	enum class Source {
		uniform,
		storage,
		vertex,
		index,
		copy,
		texelBuffer,
	};

	enum class Type {
		plain,
		struct
	};

	struct BaseType {
		Type type;	
		VkFormat plainFormat;
		std::vector<BaseType> structEntries;
	};

	struct Entry {
		BaseType* type;
		u32 arraySize {}; // 0: no array, u32(-1): dynamic sized
		std::string name {}; // might be empty
		VkDeviceSize offset {};
		VkDeviceSize stride {};
	};

	Source source;
	VkDeviceSize offset;
	VkDeviceSize range;
	std::vector<Entry> entries;
};
```

hm does it make sense in the first place to develop such a meta description
for all sources? Layout of vertex data looks very different than layout
of image src data. But hard to keep track of what is in there without
this. Maybe something like this?

```
struct BufferSection {
	// What kind of section this is
	enum class Type {
		storageUniform,
		vertex,
		index,
		imageCopy,
		bufferCopy,
		texelBuffer,
	};

	Type type;
	VkDeviceSize offset;
	VkDeviceSize size;
};

struct IndexBufferSection : BufferSection {
	VkIndexType indexType;
};

struct VertexBufferSection : BufferSection {
	std::vector<VkVertexInputAttributeDescription> attribs;
};

struct StorageUniformBufferSection : BufferSection {
	// here basically the nested entry-thing from above?
};

struct ImageCopyBufferSection : BufferSection {
	VkBufferImageCopy copy;
};

// eh, this is not really useful.
// ImageCopyBufferSection at least tells us format of pixel data.
// This could tell us something if we knew how the other buffer is used tho
struct BufferCopyBufferSection : BufferSection {
	VkBufferCopy copy;
};

struct TexelBufferSection : BufferSection {
	VkFormat format;
};

// in buffer:
std::vector<std::unique_ptr<BufferLayoutInfo>> sections;
```

---

```
select(ds, binding, elem, offset) {
	auto dstype = ds.layout->bindings[binding].descriptorType;
	auto dscat = category(dstype);

	if(dstype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || 
			dstype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
		commandHook->copyBufferBefore = binding.buffer;
	} else if(dstype == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || 
			dstype == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
		commandHook->copyImageBefore = binding.image;
	}

	// for storage images/buffers (that are not readonly) we likely
	// want to copy before *and* after, right?
	// We need both at once if we want to show "changed regions"
	// something like that
}

commandIO {
	for(auto i = 0u; i < state.descriptorSets.size(); ++i) {
		auto& ds = state.descriptorSets[i];
		if(!ds.ds) {
			imGuiText("ds {}: null", i);
			continue;
		}

		// TODO: try use ds name
		auto name = dlg::format("ds {}", i);
		if(!ImGui::TreeNode(name.c_str())) {
			continue;
		}

		for(auto b = 0u; b < ds.ds->bindings.size(); +=b) {
			auto& bindings = ds.ds->bindings[b];
			if(bindings.size() == 1) {
				int flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet;

				// TODO: show information about binding
				auto name = dlg::format("binding {}", b);
				ImGui::TreeNode(name.c_str(), flags);
				if(ImGui::IsItemClicked()) {
					select(*ds.ds, b, 0, TODO: offset);
				}

				ImGui::TreePop();
			} else {
				// TODO
			}
		}

		ImGui::TreePop();
	}
}
```

The main difficulty here atm: how to let the command hook know what
resource(s) exactly the ui wants to show.
Also, how to pool the copy dst resources in the command pool to not recreate
them for every new record.

Generic approach, where the hook itself has no idea what it is copying:

```
enum class CopyPoint {
	beforeCmd = 1 << 0,
	afterCmd = 1 << 1,
	both = (beforeCmd | afterCmd),
};

struct BufferCopyOp {
	VkBuffer buffer;
	VkDeviceSize offset;
	VkDeviceSize size;
	CopyPoint point;	
};

struct ImageCopyOp {
	VkImage image;
	VkOffset3D offset;
	VkExtent3D extent;
	VkImageSubresource subres;
	CopyPoint point;
};

// Always created on host visible memory
struct CopiedBuffer {
	Device* dev;

	VkDeviceSize size;
	VkBuffer buffer
	VkDeviceMemory memory;
	void* map;

	std::vector<Allocation> free;
};

struct CopiedBufferSpan {
	CopiedBuffer* buf;
	VkDeviceSize offset;
	VkDeviceSize size;
};

struct CopiedImage {
	Device* dev {};
	VkImage image {};
	VkImageView imageView {};
	VkDeviceMemory memory {};

	u32 width {};
	u32 height {};
	VkFormat format {};
};

struct CommandHookState {
	std::vector<CopiedImage*> imageOps;
	std::vector<CopiedBufferSpan> bufferOps;

	~CommandState();
};

struct CommandHook {
	std::vector<ImageCopyOp> imageOps;	
	std::vector<BufferCopyOp> bufferOps;	

	// ...
};
```

less generic but probably better idea, easier allowing for descriptor_indexing,
dynamic offsets and such stuff.

```
struct CommandHookState {
	std::variant<CopiedImage*, CopiedBufferAlloc> dsCopy;
	std::vector<CopiedBufferAlloc> vertexBufCopies;
	CopiedImage* attachmentCopy;
	CopiedBufferAlloc indirectCopy;
	u64 neededTime;
};

struct CommandHook {
	bool copyVertexBuffers; // could specify the needed subset in future
	bool copyIndirectDrawCmd; // always do that?
	// TODO: could add before/after specifiers
	std::optional<std::pair<unsigned, unsigned, unsigned>> copyDsState;
	std::optional<unsigned> copyAttachment; // only for cmd inside renderpass
	bool queryTime;
};
```

sketch for supporting descriptor indexing (update after bind feature)
in these command hooks:

idea 1:
- in each CommandHookRecord, store for each relevant ds (used by hooked cmd)
  the current updateCounter
- on submission: when there is a CommandHookRecord we could use, check if the
  updateCounter for all relevant descriptorSets (used by hooked command)
  still matches their latest updateCounter.
  If not, we have to create a new record.

idea 2: on UpdateDescriptor set on update-after-bind sets, we could inform
	the record that a ds was changed (maybe add it to list of ds's/commands
	that were changed). But idea 1 seems a lot better, we don't need
	this in 99% of the cases.

---
we probably want to access the returned values from CommandHook directly
in Command::displayInspect. Maybe add base class for draw/dispatch commands,
something like "StateUseCommand"?
> nah, we don't really need a class for that. Simply add function that
  display a DescriptorState object.
  For draw commands we add attachments (input, color, depthStencil)
  and vertex input as additional inputs/outputs.

---

idea for the various draw command display modes from renderdoc:
simply re-use the vertex shader and bind our own fragment shader
that draws stuff? (or, to visualize scissor/viewport just use
our own full pipelines).
We could generate the spirv manually since we don't know a priori
how many color attachments there are?
> nah, overkill. We only ever visualize one attachment so only need
  to draw to one attachment!

```
// TODO: Xlib, trying to immitate the winapi terribleness.
// Should be fixed more globally.
#ifdef Status
	#undef Status
#endif
```

----

improved submission logic:

in submit
- don't call processCB, don't hook. But build up submission, gather records
  in some list.
- then call replaceHooked(subm), where we (in case of frame-base-selection)
	- first run `match` on the selected
	  frame (up to the selected command) with the submissions up to now in 
	  this frame (including the current one).
	- if the selected command can be found in one of the currently submitted
	  records, hook it!
	  Run find on the whole record again?
	  Hm, maybe rather on the particular section (i.e. only last hierarchy part).
	  would require reworking 'find' to not require RootCommand
	- maybe replaceHooked should altogether be part of CommandHook?

```
template<typename K,
		typename Hash = std::hash<K>,
		typename Equal = std::equal_to<K>> using LinAllocScopeHashSet =
	std::unordered_set<K, Hash, Equal, LinearScopedAllocator<K>>;
```

---

			/*
			auto typeID = res->type_id;
			auto stype = &compiled.get_type(typeID);
			if(stype->pointer) {
				dlg_assert(stype->parent_type);
				typeID = stype->parent_type;
				stype = &compiled.get_type(typeID);
			}

			auto bindingCount = descriptorCount(dsState, bindingID);
			if(bindingCount > 1) {
				dlg_assert(stype->parent_type);
				typeID = stype->parent_type;
			}
			*/

---

```

bool simplify0(std::vector<ImageSubresourceLayout>& state) {
	auto changed = false;
	for(auto it = state.begin(); it != state.end();) {
		auto& sub = *it;
		if(sub.range.baseArrayLayer == 0 && sub.range.baseMipLevel == 0) {
			++it;
			continue;
		}

		auto erase = false;
		auto subLevelEnd = sub.range.baseMipLevel + sub.range.levelCount;
		auto subLayerEnd = sub.range.baseArrayLayer + sub.range.layerCount;
		for(auto& other : state) {
			if(other.range.aspectMask != sub.range.aspectMask ||
					other.layout != sub.layout) {
				continue;
			}

			auto otherLayerEnd = other.range.baseArrayLayer + other.range.layerCount;
			auto otherLevelEnd = other.range.baseMipLevel + other.range.levelCount;

			// greedy
			// lower layers
			if(sub.range.baseArrayLayer > 0 &&
					otherLayerEnd == sub.range.baseArrayLayer &&
					// 'sub' covers at least the mip range of 'other'
					other.range.baseMipLevel >= sub.range.baseMipLevel &&
					otherLevelEnd <= subLevelEnd &&
					// make sure that 'sub' is at one end of the mip range
					// of 'other' to avoid inserting additional states
					(other.range.baseMipLevel == sub.range.baseMipLevel ||
					otherLevelEnd == subLevelEnd)) {

				// can merge!
				other.range.layerCount += sub.range.layerCount;
				changed = true;

				if(other.range.baseMipLevel == sub.range.baseMipLevel &&
						other.range.levelCount == sub.range.levelCount) {
					erase = true;
				} else if(other.range.baseMipLevel != sub.range.baseMipLevel) {
					// shrink to just beginning
					sub.range.levelCount = other.range.baseMipLevel - sub.range.baseMipLevel;
				} else {
					// shrink to just end
					sub.range.levelCount = subLevelEnd - otherLevelEnd;
					sub.range.baseMipLevel = otherLevelEnd;
				}
			}

			// higher layers
			if(subLayerEnd == other.range.baseArrayLayer &&
					// 'sub' covers at least the mip range of 'other'
					other.range.baseMipLevel >= sub.range.baseMipLevel &&
					otherLevelEnd <= subLevelEnd &&
					// make sure that 'sub' is at one end of the mip range
					// of 'other' to avoid inserting additional states
					(other.range.baseMipLevel == sub.range.baseMipLevel ||
					otherLevelEnd == subLevelEnd)) {

				// can merge!
				other.range.layerCount += sub.range.layerCount;
				other.range.baseArrayLayer = sub.range.baseArrayLayer;
				changed = true;

				if(other.range.baseMipLevel == sub.range.baseMipLevel &&
						other.range.levelCount == sub.range.levelCount) {
					erase = true;
				} else if(other.range.baseMipLevel != sub.range.baseMipLevel) {
					// shrink to just beginning
					sub.range.levelCount = other.range.baseMipLevel - sub.range.baseMipLevel;
				} else {
					// shrink to just end
					sub.range.levelCount = subLevelEnd - otherLevelEnd;
					sub.range.baseMipLevel = otherLevelEnd;
				}
			}
		}

		if(erase) {
			it = state.erase(it);
		} else {
			++it;
		}
	}

	return changed;
}

bool simplify1(std::vector<ImageSubresourceLayout>& state) {
	auto changed = false;
	for(auto it = state.begin(); it != state.end();) {
		auto& sub = *it;
		if(sub.range.baseArrayLayer == 0 && sub.range.baseMipLevel == 0) {
			++it;
			continue;
		}

		auto erase = false;
		for(auto& other : state) {
			if(other.range.aspectMask != sub.range.aspectMask ||
					other.layout != sub.layout) {
				continue;
			}

			auto otherLevelEnd = other.range.baseMipLevel + other.range.levelCount;

			if(sub.range.baseMipLevel > 0 &&
					otherLevelEnd == sub.range.baseMipLevel &&
					other.range.baseArrayLayer == sub.range.baseArrayLayer &&
					other.range.layerCount == sub.range.layerCount) {
				// can merge!
				other.range.levelCount += sub.range.levelCount;
				changed = true;
				erase = true;
				break;
			}

		}

		if(erase) {
			it = state.erase(it);
		} else {
			++it;
		}
	}

	return changed;
}


void simplify(std::vector<ImageSubresourceLayout>& state) {
	// while(simplify0(state));
	// while(simplify1(state));
	// return;
```
