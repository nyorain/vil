#pragma once

#include <fwd.hpp>
#include <util/ownbuf.hpp>
#include <command/record.hpp>
#include <commandHook/state.hpp>

namespace vil {

// Internal representation of a hooked recording of a CommandRecord.
// Is kept alive only as long as the associated Record is referencing this
// (since it might resubmitted again, making this useful) or there are
// pending submission. When the record is invalidated, it no longer references
// the record.
// Since the record must stay alive and valid until all submissions have
// completed, we can assume the Record this hook was created for remains
// valid throughout its lifetime.
struct CommandHookRecord {
	// Associated hook. Might be null if this was invalidated
	// TODO: we don't really need this, can just use dev->commandHook.
	CommandHook* hook {};
	CommandRecord* record {}; // the record we hook. Always valid.
	LocalCapture* localCapture {}; // when this was hooked for a local capture
	u32 hookCounter {}; // hook->counter_ at creation time; for invalidation
	// Hierachy of the hooked command. May be empty when there was no
	// selected command and this HookRecord just exists for accelStructs
	std::vector<const Command*> hcommand;
	float match {}; // how much the original command matched the searched one

	// When there is currently a (hook) submission using this record,
	// it is stored here. Synchronized via device mutex.
	Submission* writer {};

	// When copying state from a descriptor set, this will hold the pointer
	// to the associated descriptor set state. Used for updateAfterBind
	// descriptors: when they change we must recreate the CommandHookRecord.
	// One state pointer for each descriptorCopy stored here.
	// TODO: temporary removal of record.dsState due to sync issues.
	// Needs to be fixed!
	// std::vector<IntrusivePtr<DescriptorSetCow>> dsState;

	// == Resources ==
	VkCommandBuffer cb {};

	// PERF: allocate resources from pool instead of giving each record
	// its entirely own set of resources (e.g. queryPool and images/buffers
	// in CommandHookState).
	VkQueryPool queryPool {};

	// When the viewed command is inside a render pass and we need to
	// perform transfer operations before/after it, we need to split
	// up the render pass.
	VkRenderPass rp0 {};
	VkRenderPass rp1 {};
	VkRenderPass rp2 {};

	IntrusivePtr<CommandHookState> state {};
	OwnBuffer dummyBuf {};

	// AccelStruct-related stuff.
	// We need to hook every CmdBuildAccelerationStructure, making sure
	// we store the state the accelStruct is built with.
	struct AccelStructBuild {
		const Command* command {};

		struct Build {
			AccelStruct* dst;
			AccelStructStatePtr state;
		};

		std::vector<Build> builds;
	};

	struct AccelStructCapture {
		unsigned id; // index into state->copiedDescriptors
		AccelStruct* accelStruct;
	};

	struct AccelStructCopy {
		AccelStruct* src {};
		AccelStruct* dst {};
		AccelStructStatePtr state {}; // filled in later by CommandHookSubmission
	};

	using AccelStructOp = std::variant<
		AccelStructBuild,
		AccelStructCapture,
		AccelStructCopy
	>;

	// Order here is important, ops might depend on each other
	std::vector<AccelStructOp> accelStructOps;

	// Needed for image to buffer sample-copying
	std::vector<VkDescriptorSet> descriptorSets;
	std::vector<VkImageView> imageViews;
	std::vector<VkBufferView> bufferViews;

	// Linked list of all records belonging to this->hook
	CommandHookRecord* next {};
	CommandHookRecord* prev {};

public:
	CommandHookRecord(CommandHook& hook, CommandRecord& record,
		std::vector<const Command*> hooked,
		const CommandDescriptorSnapshot& descriptors,
		const CommandHookOps& ops, LocalCapture* localCapture = nullptr);
	~CommandHookRecord();

	// Called when associated record is destroyed or hook replaced.
	// Called while device mutex is locked.
	// Might delete itself (or decrement reference count or something).
	void finish() noexcept;

	// Returns whether this command has an associated hooked command.
	// There are HookRecord that don't have an associated hooked command
	// if we need to perform custom commands, e.g. for accelStruct building.
	bool hasHookedCmd() const { return !hcommand.empty(); }

private:
	struct RecordInfo {
		const CommandHookOps& ops;

		bool splitRendering {}; // whether we have to hook the renderpass
		u32 hookedSubpass {};
		const BeginRenderPassCmd* beginRenderPassCmd {};
		const BeginRenderingCmd* beginRenderingCmd {};
		const RenderPassInstanceState* rpi {};
		const CommandDescriptorSnapshot* descriptors {};

		unsigned nextHookLevel {}; // on hcommand, hook hierarchy
		unsigned* maxHookLevel {};

		bool rebindComputeState {};
	};

	void initState(RecordInfo&);

	// = Recording =
	// Will record the given command. Uses the given RecordInfo to do
	// additional operations if needed (such as rebinding the compute state,
	// if needed).
	void dispatchRecord(Command& cmd, RecordInfo&);

	// Called when we arrived ath the hooked command itself. Will make sure
	// all barriers are set, render passes split correclty and copies are done.
	void hookRecordDst(Command& dst, RecordInfo&);

	// Called immediately before recording the hooked command itself.
	// Will perform all needed operations.
	void hookRecordBeforeDst(Command& dst, RecordInfo&);

	// Called immediately after recording the hooked command itself.
	// Will perform all needed operations.
	void hookRecordAfterDst(Command& dst, RecordInfo&);

	// Recursively records the given linked list of commands.
	void hookRecord(Command* cmdChain, RecordInfo&);

	// Returns the state of the *last* AccelStruct build for the acceleration
	// structure at the given address, or null if there is none.
	IntrusivePtr<AccelStructState> lastAccelStructBuild(u64 accelStructAddress);

	// = Copying =
	void copyTransfer(Command& bcmd, RecordInfo&, bool isBefore);
	void copyDs(Command& bcmd, RecordInfo&,
		const DescriptorCopyOp&, unsigned copyDstID,
		CommandHookState::CopiedDescriptor& dst,
		IntrusivePtr<DescriptorSetCow>& dstCow);
	void copyAttachment(const Command& bcmd, const RecordInfo&,
		AttachmentType type, unsigned id,
		CommandHookState::CopiedAttachment& dst);
	void beforeDstOutsideRp(Command&, RecordInfo&);
	void afterDstOutsideRp(Command&, RecordInfo&);

	void hookBefore(const BuildAccelStructsCmd&);
	void hookBefore(const BuildAccelStructsIndirectCmd&);

	// TODO: kinda arbitrary, allow more. Configurable via settings?
	// In general, the problem is that we can't know the relevant
	// size for sub-allocated buffers. Theoretically, we could analyze
	// previous index/indirect data for this. Not sure if good idea.
	// NOTE: we should probably just set the required offset/size via
	// gui, implement some paging mechanism for buffer and vertex
	// viewer. Even if that means we have a frame delay
	static constexpr auto maxBufCopySize = VkDeviceSize(2 * 1024 * 1024);

	// TODO: should the whole buffer be copied for transfer operations?
	// bad idea in many cases, e.g. when huge upload heaps are used.
	static constexpr auto copyFullTransferBuffer = false;

	// See node 1963
	static constexpr auto timingBarrierBefore = true;
	static constexpr auto timingBarrierAfter = true;
};


} // namespace vil
