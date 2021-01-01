#pragma once

#include <fwd.hpp>
#include <queue.hpp>

namespace fuen {

struct CommandHookRecordImpl;
struct BeginRenderPassCmd;

struct CommandHookImpl : CommandHook {
public:
	// timing
	u64 lastTime {};

	// indirect commands
	struct {
		u32 count;
		std::vector<std::byte> data;
	} indirect;

	// image before command
	// VkImageView lastImageView {};

public:
	VkCommandBuffer hook(CommandBuffer& hooked,
		FinishPtr<CommandHookSubmission>& data) override;

	void desc(std::vector<CommandDesc> desc);
	void invalidateRecordings();

	void finish() noexcept override { delete this; }

	~CommandHookImpl();

private:
	friend struct CommandHookRecordImpl;
	u32 counter_ {0};
	std::vector<CommandDesc> desc_ {};
	CommandHookRecordImpl* records_ {}; // linked list
};

struct CommandHookRecordImpl : CommandHookRecord {
	CommandHookImpl* hook {};
	CommandRecord* record {}; // the record we hook
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

	VkImage dstImage {};
	VkImageView dstImageView {};
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
		unsigned rpSubpass {}; // current subpass
		BeginRenderPassCmd* beginRenderPassCmd {};

		unsigned nextHookLevel {}; // on hcommand, hook hierarchy
	};

	void hookRecord(Command* cmdChain, RecordInfo info);

	void beforeDstOutsideRp(Command& cmd);
	void afterDstOutsideRp(Command& cmd);

	void finish() noexcept override;
};

struct CommandHookSubmissionImpl : CommandHookSubmission {
	CommandHookRecordImpl* record {};

	CommandHookSubmissionImpl(CommandHookRecordImpl& rec) : record(&rec) {}
	~CommandHookSubmissionImpl();

	void finish() noexcept override { delete this; }
	void transmitTiming();
	void transmitIndirect();
};

} // namespace fuen
