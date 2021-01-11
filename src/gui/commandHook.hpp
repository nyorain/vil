#pragma once

#include <fwd.hpp>
#include <queue.hpp>

namespace fuen {

struct CommandHookRecordImpl;
struct BeginRenderPassCmd;

struct ViewableImageCopy {
	Device* dev {};
	u32 refCount {};
	VkImage image {};
	VkImageView imageView {};
	VkDeviceMemory memory {};
	PendingSubmission* writer {};

	u32 width {};
	u32 height {};

	ViewableImageCopy(Device& dev, VkFormat, u32 width, u32 height);
	~ViewableImageCopy();
};

struct CommandHookImpl : CommandHook {
public:
	// timing
	u64 lastTime {};

	// indirect commands
	struct {
		u32 count {};
		std::vector<std::byte> data;
	} indirect;

	IntrusivePtr<ViewableImageCopy> image;

public:
	VkCommandBuffer hook(CommandBuffer& hooked,
		PendingSubmission& subm,
		FinishPtr<CommandHookSubmission>& data) override;

	void desc(std::vector<CommandDesc> desc);
	void invalidateRecordings();
	void invalidateData() {
		lastTime = {};
		indirect = {};
		image = {};
	}

	void finish() noexcept override { delete this; }

	~CommandHookImpl();

private:
	friend struct CommandHookRecordImpl;
	u32 counter_ {0};
	std::vector<CommandDesc> desc_ {};
	CommandHookRecordImpl* records_ {}; // linked list
};

// Is kept alive only as long as the associated Record is referencing this
// (since it might resubmitted again, making this useful) or there are
// pending submission. When the record is invalidated, it no longer references
// the record.
// Since the record must stay alive and valid until all submissions have
// completed, we can assume the Record this hook was created for remains
// valid throughout its lifetime.
struct CommandHookRecordImpl : CommandHookRecord {
	CommandHookImpl* hook {}; // Associated hook. Might be null if this was invalidated
	CommandRecord* record {}; // the record we hook. Always valid.
	u32 hookCounter {}; // hook->counter_ at creation time; for invalidation
	std::vector<Command*> hcommand; // hierachy of the hooked command

	// == Resources ==
	VkCommandBuffer cb {};

	// TODO: allocate resources from pool instead of giving each record
	// its entirely own set of resources.
	VkQueryPool queryPool {};
	u32 submissionCount {0}; // comparable to ref count

	// When the viewed command is inside a render pass and we need to
	// perform transfer operations before/after it, we need to split
	// up the render pass.
	VkRenderPass rp0 {};
	VkRenderPass rp1 {};
	VkRenderPass rp2 {};

	IntrusivePtr<ViewableImageCopy> dstImage {};

	VkBuffer dstBuffer {};
	void* bufferMap {};
	VkDeviceMemory dstMemory {};

	// Linked list of all records belonging to this->hook
	CommandHookRecordImpl* next {};
	CommandHookRecordImpl* prev {};

	CommandHookRecordImpl(CommandHookImpl& hook, CommandRecord& record,
		std::vector<Command*> hooked);
	~CommandHookRecordImpl();

	struct RecordInfo {
		bool splitRenderPass {}; // whether we have to hook the renderpass
		u32 hookedSubpass {};
		BeginRenderPassCmd* beginRenderPassCmd {};

		unsigned nextHookLevel {}; // on hcommand, hook hierarchy
	};

	void hookRecord(Command* cmdChain, RecordInfo info);

	void beforeDstOutsideRp(Command& cmd, const RecordInfo& info);
	void afterDstOutsideRp(Command& cmd, const RecordInfo& info);

	void finish() noexcept override;
};

struct CommandHookSubmissionImpl : CommandHookSubmission {
	CommandHookRecordImpl* record {};

	CommandHookSubmissionImpl(CommandHookRecordImpl&, PendingSubmission&);
	~CommandHookSubmissionImpl();

	void finish() noexcept override { delete this; }
	void transmitTiming();
	void transmitIndirect();
};

} // namespace fuen
