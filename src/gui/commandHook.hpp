#pragma once

#include <fwd.hpp>
#include <cow.hpp>
#include <command/desc.hpp>
#include <command/record.hpp>
#include <util/intrusive.hpp>
#include <util/bytes.hpp>
#include <util/ownbuf.hpp>
#include <gui/render.hpp>
#include <vk/vulkan.h>
#include <vector>
#include <memory>
#include <variant>
#include <optional>
#include <string>

namespace vil {

struct BeginRenderPassCmd;
struct CommandHookState;

// Commandbuffer hook that allows us to forward a modified version
// of this command buffer down the chain. Only called during submission,
// when the given CommandBuffer has a valid recording.
struct CommandHook {
public:
	// Mainly useful for debugging, should always be true otherwise
	// as we need it to have accelStruct data.
	static constexpr bool hookAccelStructBuilds = true;

	struct DescriptorCopy {
		unsigned set {};
		unsigned binding {};
		unsigned elem {};
		bool before {}; // whether to copy before or after target command

		// If this is set to true, image data will be copied into a host-mapped
		// buffer as well. Useful to reading the data back on cpu instead
		// of displaying it.
		bool imageAsBuffer {};
	};

	struct AttachmentCopy {
		AttachmentType type;
		unsigned id;
		bool before {}; // whether to copy before or after target command
	};

	// Defines what to hook
	struct {
		bool all {};
		CommandBuffer* cb {};
		CommandRecord* record {};
	} target;

	// Temporarily don't hook commands even if hook ops are set
	// and a submission matches a target.
	// Note that in comparison to just unsetting hook ops and target,
	// this allows all already hooked records to stay valid.
	bool freeze {};

	// Always hooks, even with disabled gui. Mainly for testing.
	bool forceHook {};

	// Which operations/state copies to peform.
	// When updating e.g. the id of the ds to be copied, all existing
	// recordings have to be invalidated!
	bool copyVertexBuffers {}; // could specify the needed subset in future
	bool copyIndexBuffers {};
	bool copyXfb {}; // transform feedback
	bool copyIndirectCmd {};
	std::vector<DescriptorCopy> descriptorCopies;
	std::vector<AttachmentCopy> attachmentCopies; // only for cmd inside renderpass
	bool queryTime {};

	// transfer
	// NOTE: copySrc and copyDst can't be true at same time, atm
	bool copyTransferSrc {};
	bool copyTransferDst {};
	bool copyTransferBefore {};
	u32 transferIdx {}; // the relevant region/blit/attachment of the transfer command

	// A vector of the last received states of finished submissions.
	// Must be reset manually when retrieved.
	struct CompletedHook {
		u64 submissionID; // global submission id (dev.submissionCounter)
		IntrusivePtr<CommandRecord> record;
		IntrusivePtr<CommandHookState> state;
		CommandDescriptorSnapshot descriptorSnapshot;
		std::vector<const Command*> command;
		float match; // how much the command matched
	};

	std::vector<CompletedHook> completed;

public:
	CommandHook(Device& dev);
	~CommandHook();

	// Called from inside QueueSubmit with the command buffer the hook has
	// been installed for. Can therefore expect the command buffer to be
	// in executable state.
	// Can return the command buffer handle itself or a hooked one but
	// must return a valid command buffer.
	// Called while device mutex is locked.
	// Additionally has the possibility to return something in 'data' that
	// gets associated with the lifetime of the submission (i.e. is destroyed
	// when the submission is finished).
	VkCommandBuffer hook(CommandBuffer& hooked,
		Submission& subm, std::unique_ptr<CommandHookSubmission>& data);

	void invalidateRecordings(bool forceAll = false);
	void invalidateData() { completed.clear(); }

	// invalidate: Automatically invalidates data and recordings?
	void desc(IntrusivePtr<CommandRecord> rec, std::vector<const Command*> hierachy,
		CommandDescriptorSnapshot, bool invalidate = true);
	void unsetHookOps(bool doQueryTime = false);

	const auto& dsState() const { return dsState_; }
	auto recordPtr() const { return record_; }

private:
	// Initializes the pipelines and data needed for acceleration
	// structure copies
	void initAccelStructCopy(Device& dev);
	void initImageCopyPipes(Device& dev);

	// Checks whether the copied descriptors in the associated
	// record have changed (via update-after-bind) since the hooked
	// record was created. Exepcts the given record to be valid.
	bool copiedDescriptorChanged(const CommandHookRecord&);

private:
	friend struct CommandHookRecord;

	Device* dev_ {};

	u32 counter_ {0};
	CommandHookRecord* records_ {}; // intrusive linked list

	// description of command to be hooked
	IntrusivePtr<CommandRecord> record_;
	CommandDescriptorSnapshot dsState_;
	std::vector<const Command*> hierachy_;

	// pipelines needed for the acceleration structure build copy
public: // TODO, for cow. Maybe just move them to Device?
	VkPipelineLayout accelStructPipeLayout_ {};
	VkPipeline accelStructVertCopy_ {};

	// pipeline for sample-copying image to buffer
	VkDescriptorSetLayout copyImageDsLayout_ {};
	VkPipelineLayout copyImagePipeLayout_ {};
	VkPipeline copyImagePipes_[ShaderImageType::count] {};
};

// Collection of data we got out of a submission/command.
struct CommandHookState {
	struct CopiedDescriptor {
		std::variant<
			// No descriptor copied.
			std::monostate,
			// The added cow or already resolved copy.
			IntrusivePtr<CowImageRange>,
			// The copied buffer.
			// NOTE: will be made CowBufferRange ptr in future
			OwnBuffer
		> data;
	};

	struct CopiedAttachment {
		CopiedImage data;
	};

	// We need a reference count here since this object is conceptually owned by
	// CommandRecord but may be read by the gui even when the record
	// was already destroyed (e.g. because it was replaced and all submissions
	// have finished).
	std::atomic<u32> refCount {};

	// Time needed for the given command.
	// Set to u64(-1) on error.
	u64 neededTime {u64(-1)};

	std::vector<CopiedDescriptor> copiedDescriptors;
	std::vector<CopiedAttachment> copiedAttachments;

	// For indirect commands: holds a copy of the indirect command(s)
	u32 indirectCommandCount {};
	OwnBuffer indirectCopy {};

	// Only for draw commands
	std::vector<OwnBuffer> vertexBufCopies {}; // draw cmd: Copy of all vertex buffers
	OwnBuffer indexBufCopy {}; // draw cmd: Copy of index buffer
	OwnBuffer transformFeedback {}; // draw cmd: position output of vertex stage

	// Only for transfer commands
	OwnBuffer transferBufCopy {};
	CopiedImage transferImgCopy {};

	CommandHookState();
	~CommandHookState();
};

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
	std::vector<IntrusivePtr<DescriptorSetCow>> dsState;

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

	// The CommandHookState owned by this
	IntrusivePtr<CommandHookState> state {};
	OwnBuffer dummyBuf {};

	// AccelStruct-related stuff.
	// We need to hook every CmdBuildAccelerationStructure, making sure
	// we store the state the accelStruct is built with.
	struct AccelStructBuild {
		const Command* command {};

		struct Build {
			AccelStruct* dst;
			VkAccelerationStructureBuildGeometryInfoKHR info;
			span<const VkAccelerationStructureBuildRangeInfoKHR> rangeInfos;
			std::vector<VkAccelerationStructureGeometryKHR> geoms;
		};

		std::vector<Build> builds;
	};

	std::vector<AccelStructBuild> accelStructBuilds;

	// Needed for image to buffer sample-copying
	std::vector<VkDescriptorSet> descriptorSets;
	std::vector<VkImageView> imageViews;

	// Linked list of all records belonging to this->hook
	CommandHookRecord* next {};
	CommandHookRecord* prev {};

public:
	CommandHookRecord(CommandHook& hook, CommandRecord& record,
		std::vector<const Command*> hooked,
		const CommandDescriptorSnapshot& descriptors);
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
		bool splitRenderPass {}; // whether we have to hook the renderpass
		u32 hookedSubpass {};
		const BeginRenderPassCmd* beginRenderPassCmd {};
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

	// = Copying =
	void copyTransfer(Command& bcmd, RecordInfo&);
	void copyDs(Command& bcmd, RecordInfo&,
		const CommandHook::DescriptorCopy&,
		CommandHookState::CopiedDescriptor& dst,
		IntrusivePtr<DescriptorSetCow>& dstCow);
	void copyAttachment(const Command& bcmd,
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
	static constexpr auto maxBufCopySize = VkDeviceSize(32 * 1024 * 1024);

	// TODO: should the whole buffer be copied for transfer operations?
	// bad idea in many cases, e.g. when huge upload heaps are used.
	static constexpr auto copyFullTransferBuffer = false;

	// See node 1963
	static constexpr auto timingBarrierBefore = true;
	static constexpr auto timingBarrierAfter = true;
};

struct CommandHookSubmission {
	CommandHookRecord* record {};
	CommandDescriptorSnapshot descriptorSnapshot {};

	CommandHookSubmission(CommandHookRecord&, Submission&,
		CommandDescriptorSnapshot descriptors);
	~CommandHookSubmission();

	// Called when the associated submission (passed again as parameter)
	// successfully completed execution on the device.
	void finish(Submission&);
	void finishAccelStructBuilds();
	void transmitTiming();
	void transmitIndirect();
};

} // namespace vil
