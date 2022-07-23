#pragma once

#include <fwd.hpp>
#include <cow.hpp>
#include <commandHook/state.hpp>
#include <command/desc.hpp>
#include <command/record.hpp>
#include <util/intrusive.hpp>
#include <util/bytes.hpp>
#include <util/ownbuf.hpp>
#include <util/util.hpp>
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
	std::vector<DescriptorCopyOp> descriptorCopies;
	std::vector<AttachmentCopyOp> attachmentCopies; // only for cmd inside renderpass
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
	CommandHookState* stillNeeded {}; // hookState that can't be recycled

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

} // namespace vil
