#pragma once

#include <fwd.hpp>
#include <commandHook/state.hpp>
#include <command/record.hpp>
#include <util/intrusive.hpp>
#include <nytl/bytes.hpp>
#include <util/ownbuf.hpp>
#include <util/util.hpp>
#include <frame.hpp>
#include <cb.hpp>
#include <vk/vulkan.h>
#include <vector>
#include <memory>
#include <variant>
#include <optional>
#include <string>

namespace vil {

struct BeginRenderPassCmd;
struct CommandHookState;

enum class CommandHookTargetType {
	none,
	// Hook any submission that matches
	all,
	// Hook any submission that matches inside the given frame context
	inFrame,
	// Just hook a specific command buffer
	commandBuffer,
	// Just hook the record specified by desc()
	commandRecord,
};

// Defines what to hook
struct CommandHookTarget {
	CommandHookTargetType type {CommandHookTargetType::none};
	// Can be empty only when type == inFrame, in which case only a
	// timing query is allowed as hookOp, which will then time the
	// whole submission.
	IntrusivePtr<CommandRecord> record {};
	// When hierarchy is empty, can only have a timing query as hookOp,
	// which will then time the whole command buffer.
	// If not empty, must be a valid hierarchy in 'record'.
	std::vector<const Command*> command {};
	// Descriptors for the selected command.
	CommandDescriptorSnapshot descriptors {};

	// only for type == commandBuffer
	CommandBufferPtr cb {};

	// only for type == inFrame
	std::vector<FrameSubmission> frame {};
	// Must be in bounds of 'frame' and not u32(-1) when type == inFrame
	u32 submissionID {u32(-1)};
};

// Defines which data to retrieve
struct CommandHookOps {
	// Which operations/state copies to peform.
	// When updating e.g. the id of the ds to be copied, all existing
	// recordings have to be invalidated!
	bool copyVertexBuffers {}; // could specify the needed subset in future
	bool copyIndexBuffers {};
	bool copyXfb {}; // transform feedback
	bool copyIndirectCmd {};
	std::vector<DescriptorCopyOp> descriptorCopies;
	std::vector<AttachmentCopyOp> attachmentCopies; // only for cmd inside renderpass
	bool queryTime {};

	// transfer
	bool copyTransferSrcBefore {};
	bool copyTransferSrcAfter {};
	bool copyTransferDstBefore {};
	bool copyTransferDstAfter {};

	// TODO: allow setting this to u32(-1), not supported atm
	// The relevant region/blit/attachment of the transfer command
	// Pass u32(-1) to just copy everything, independent of transfers

	u32 transferIdx {0u};
};

// Only set the members that should be updated.
// We have this instead of separate functions for target and ops
// to make the update atomic.
struct CommandHookUpdate {
	std::optional<CommandHookTarget> newTarget;
	std::optional<CommandHookOps> newOps;
	// Whether to automatically clear completed hooks and invalidate recordings.
	// Must be true if ops are updated.
	bool invalidate {};
};

// A vector of the last received states of finished submissions.
struct CompletedHook {
	u64 submissionID; // global submission id (dev.submissionCounter)
	IntrusivePtr<CommandRecord> record;
	IntrusivePtr<CommandHookState> state;
	CommandDescriptorSnapshot descriptorSnapshot;
	std::vector<const Command*> command;
	float match; // how much the command matched
};

enum class LocalCaptureBits : u32 {
	// Capture all data needed for shader debugging
	shaderDebugger = (1u << 0u),
	// Capture all (before and after) descriptor data
	descriptors = (1u << 1u),
	// captures attachments (only relevant for draw cmds)
	attachments = (1u << 2u),
	// capture transfer I/O before command (only relevant for transfer cmds)
	transferBefore = (1u << 3u),
	// capture transfer I/O before command (only relevant for transfer cmds)
	transferAfter = (1u << 4u),
	// capture vertex/index buffer data
	vertexInput = (1u << 5u),
	// capture vertex shader output data (via xfb)
	vertexOutput = (1u << 6u),

	allCapture = (1u << 10u) - 1u,

	// Only do a single capture, unregister the hook afterwards.
	once = (1u << 10u),

	// Keep the history of all done hooks.
	// Extremely expensive (keeps a lot of memory around) if the command
	// is submitted often.
	// Does only make sense when 'once' is not set.
	// keepAll = (1u << 8u),
};

NYTL_FLAG_OPS(LocalCaptureBits)

std::string_view name(LocalCaptureBits localCaptureBit);
std::optional<LocalCaptureBits> localCaptureBit(std::string_view name);

// Lifetime of LocalCapture objects: once created they live forever.
struct LocalCapture {
	// immutable
	LocalCaptureFlags flags;
	std::string name;

	// Protected by device mutex.
	// NOTE: record, command might not always mirror completex.{record, command}.
	//   LocalCaptures without once flags can be updated with more
	//   recent recordings while we don't have a completed hook state
	//   of them yet.
	IntrusivePtr<CommandRecord> record;
	std::vector<const Command*> command;
	CompletedHook completed; // may be empty
};

// Commandbuffer hook that allows us to forward a modified version
// of this command buffer down the chain. Only called during submission,
// when the given CommandBuffer has a valid recording.
struct CommandHook {
public:
	// maximum number of completed hooks we store at a time.
	static constexpr auto maxCompletedHooks = 8u;

	// TODO: make setting?
	static constexpr auto matchType = MatchType::mixed;

	using TargetType = CommandHookTargetType;
	using Target = CommandHookTarget;
	using Ops = CommandHookOps;
	using Update = CommandHookUpdate;

	// Temporarily don't hook commands even if hook ops are set
	// and a submission matches a target.
	// Note that in comparison to just unsetting hook ops and target,
	// this allows all already hooked records to stay valid.
	std::atomic<bool> freeze {};

	// Always hooks, even with disabled gui. Mainly for testing.
	std::atomic<bool> forceHook {};

	// Never re-uses hooked records, even if possible. Mainly for debugging.
	std::atomic<bool> allowReuse {true};

	// Mainly useful for debugging, should always be true otherwise
	// as we need it to have accelStruct data.
	std::atomic<bool> hookAccelStructBuilds {true};

public:
	CommandHook(Device& dev);
	~CommandHook();

	// Called from inside QueueSubit, before dispatching the submit call.
	// Can expect all command buffers (and their records) to be valid and
	// in executable state.
	// The hook can replace commandBuffers and add attach CommandHookSubmission
	// objects to the associated cb submissions.
	// Called with the device mutex unlocked.
	// Assumes that the QueueSubmitter is a QueueSubmit command (i.e.
	// not QueueBindSparse).
	void hook(QueueSubmitter& subm);

	// Updates the hook operations
	void updateHook(Update&& update);

	// Moves all completed hooks to the caller, clearing them
	// internally.
	[[nodiscard]] std::vector<CompletedHook> moveCompleted();

	// NOTE: copies are being made here (inside a critical section)
	// so these functions are more expensive than simple getters.
	Ops ops() const;
	Target target() const;

	void invalidateRecordings(bool forceAll = false);
	void clearCompleted();

	void addLocalCapture(std::unique_ptr<LocalCapture>&&);
	std::vector<LocalCapture*> localCaptures() const;
	std::vector<LocalCapture*> localCapturesOnceCompleted() const;

private:
	// Initializes the pipelines and data needed for acceleration
	// structure copies
	void initAccelStructCopy(Device& dev);
	void initImageCopyPipes(Device& dev);

	// Checks whether the copied descriptors in the associated
	// record have changed (via update-after-bind) since the hooked
	// record was created. Exepcts the given record to be valid.
	bool copiedDescriptorChanged(const CommandHookRecord&);

	VkCommandBuffer doHook(CommandRecord& record,
		span<const Command*> dstCommand, // might be empty
		float dstCommandMatch,
		Submission& subm, std::unique_ptr<CommandHookSubmission>& data,
		LocalCapture* localCapture = nullptr);

	VkCommandBuffer hook(CommandRecord& record,
		span<const CommandSectionMatch> matchData,
		Submission& subm, std::unique_ptr<CommandHookSubmission>& data);

private:
	friend struct CommandHookRecord;
	friend struct CommandHookSubmission;

	Device* dev_ {};

	u32 counter_ {0};
	CommandHookRecord* records_ {}; // intrusive linked list
									//
	std::vector<CompletedHook> completed_;
	Ops ops_;
	Target target_;
	LinAllocator matchAlloc_;

	std::vector<std::unique_ptr<LocalCapture>> localCaptures_;
	// LocalCaptures with 'once' flag set that were completed.
	// Stored as extra list so we don't have to check them every time.
	std::vector<std::unique_ptr<LocalCapture>> localCapturesCompleted_;

	// TODO: wip hack
	std::vector<CompletedHook> keepAliveLC_;

	// pipelines needed for the acceleration structure build copy
public: // TODO, for copying. Maybe just move them to Device?
	VkPipelineLayout accelStructPipeLayout_ {};
	VkPipeline accelStructVertCopy_ {};

	// pipeline for sample-copying image to buffer
	VkDescriptorSetLayout sampleImageDsLayout_ {};
	VkPipelineLayout sampleImagePipeLayout_ {};
	VkPipeline sampleImagePipes_[ShaderImageType::count] {};
};

} // namespace vil
