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
