#pragma once

#include <fwd.hpp>
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

struct CopiedImage {
	Device* dev {};
	u32 refCount {};
	VkImage image {};
	VkDeviceMemory memory {};
	VkExtent3D extent {}; // extent of the first level in this image
	u32 layerCount {};
	u32 levelCount {};
	VkImageAspectFlags aspectMask {};
	VkDeviceSize neededMemory {};

	// TODO(io-rework): shouldn't be here. Subresource range of original src image,
	// that this copy was created from.
	VkImageSubresourceRange srcSubresRange {};
	VkFormat format {};

	CopiedImage() = default;
	[[nodiscard]] bool init(Device& dev, VkFormat, const VkExtent3D&,
		u32 layers, u32 levels, VkImageAspectFlags aspects, u32 srcQueueFam);
	~CopiedImage();
};

struct CommandHookState;

// Commandbuffer hook that allows us to forward a modified version
// of this command buffer down the chain. Only called during submission,
// when the given CommandBuffer has a valid recording.
struct CommandHook {
public:
	struct DescriptorCopy {
		unsigned set {};
		unsigned binding {};
		unsigned elem {};
		bool before {}; // whether to copy before or after target command
	};

	struct AttachmentCopy {
		unsigned id;
		bool before {}; // whether to copy before or after target command
	};

	// Defines what to hook
	struct {
		bool all {};
		CommandBuffer* cb {};
		CommandRecord* record {};
	} target;

	bool freeze {}; // temporarily don't resubmit states

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

	// Called when hook is removed from command buffer or command group.
	// Called while device mutex is locked.
	// Might delete itself (or decrement reference count or something).
	void finish() noexcept { delete this; }

	void invalidateRecordings();
	void invalidateData() { completed.clear(); }

	// invalidate: Automatically invalidates data and recordings?
	void desc(IntrusivePtr<CommandRecord> rec, std::vector<const Command*> hierachy,
		CommandDescriptorSnapshot, bool invalidate = true);
	void unsetHookOps(bool doQueryTime = false);

	const auto& dsState() const { return dsState_; }

private:
	// Initializes the pipelines and data needed for acceleration
	// structure copies
	void initAccelStructCopy(Device& dev);

	// Checks whether the copied descriptors in the associated
	// record have changed (via update-after-bind) since the hooked
	// record was created.
	bool copiedDescriptorChanged(const CommandHookRecord&);

private:
	Device* dev_ {};

	friend struct CommandHookRecord;
	u32 counter_ {0};
	CommandHookRecord* records_ {}; // intrusive linked list

	// description of command to be hooked
	IntrusivePtr<CommandRecord> record_;
	CommandDescriptorSnapshot dsState_;
	std::vector<const Command*> hierachy_;

	// pipelines needed for the acceleration structure build copy
	VkPipelineLayout accelStructPipeLayout_ {};
	VkPipeline accelStructVertCopy_ {};
};

// Collection of data we got out of a submission/command.
struct CommandHookState {
	struct CopiedDescriptor {
		std::variant<std::monostate, CopiedImage, OwnBuffer> data;
	};

	struct CopiedAttachment {
		CopiedImage data;
	};

	// We need a reference count here since this object is conceptually owned by
	// CommandRecord but may be read by the gui even when the record
	// was already destroyed (e.g. because it was replaced and all submissions
	// have finished).
	u32 refCount {};
	u64 neededTime {u64(-1)}; // time needed for the given command

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
	DescriptorSetStatePtr dsState;

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
			VkAccelerationStructureBuildGeometryInfoKHR info;
			span<const VkAccelerationStructureBuildRangeInfoKHR> rangeInfos;
			std::vector<VkAccelerationStructureGeometryKHR> geoms;
		};

		std::vector<Build> builds;
	};

	std::vector<AccelStructBuild> accelStructBuilds;

	// Linked list of all records belonging to this->hook
	CommandHookRecord* next {};
	CommandHookRecord* prev {};

public:
	CommandHookRecord(CommandHook& hook, CommandRecord& record,
		std::vector<const Command*> hooked);
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

		unsigned nextHookLevel {}; // on hcommand, hook hierarchy
		unsigned* maxHookLevel {};
	};

	void initState(RecordInfo&);

	void hookRecordBeforeDst(Command& dst, const RecordInfo&);
	void hookRecordAfterDst(Command& dst, const RecordInfo&);
	void hookRecordDst(Command& dst, const RecordInfo&);
	void hookRecord(Command* cmdChain, const RecordInfo&);

	void copyTransfer(Command& bcmd, const RecordInfo&);
	void copyDs(Command& bcmd, const RecordInfo&,
		const CommandHook::DescriptorCopy&,
		CommandHookState::CopiedDescriptor& dst);
	void copyAttachment(const RecordInfo&, unsigned id,
		CommandHookState::CopiedAttachment& dst);
	void beforeDstOutsideRp(Command&, const RecordInfo&);
	void afterDstOutsideRp(Command&, const RecordInfo&);

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

	CommandHookSubmission(CommandHookRecord&, Submission&);
	~CommandHookSubmission();

	// Called when the associated submission (passed again as parameter)
	// successfully completed execution on the device.
	void finish(Submission&);
	void finishAccelStructBuilds();
	void transmitTiming();
	void transmitIndirect();
};

} // namespace vil
