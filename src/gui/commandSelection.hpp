#pragma once

#include <fwd.hpp>
#include <util/intrusive.hpp>
#include <commandHook/state.hpp>
#include <util/syncedMap.hpp> // TODO: get rid of include, only for CommandBufferPtr
#include <command/record.hpp> // TODO: get rid of include, only for CommandDescriptorSnapshot
#include <frame.hpp>
#include <vector>

namespace vil {

// Manages the currently selected command and finding it in new submissions
// and retrieving the approriate state from completed hooks.
class CommandSelection {
public:
	// Defines from which source the selected state is updated.
	enum class UpdateMode {
		none, // does not update them at all, just select the static record
		localCapture, // same as none but uses a fixed state
		commandBuffer, // update them from the commands in the selected command buffer
		any, // update them from any submitted matching record
		swapchain, // update them from the active swapchain
	};

	enum class SelectionType {
		none,
		command,
		submission, // only in swapchain mode
		record, // only in swapchain mode
	};

	// Whether to freeze the selected state.
	// New completed hooks will still be fetched when something new is
	// selected but once we have a state we don't gather more and don't
	// update the selection.
	bool freezeState {};

public:
	CommandSelection();
	~CommandSelection();

	void init(Device& dev) { dev_ = &dev; }

	// Tries to fetch a new completed hook, updating the selection.
	// Returns whether something was updated.
	bool update();

	// Sets updateMode to 'none'
	// 'cmd' must be empty or a valid hierarchy inside 'record'
	void select(IntrusivePtr<CommandRecord> record,
		std::vector<const Command*> cmd);

	// Sets updateMode to swapchain
	// 'submission' must be u32(-1) or in the bounds of 'frame'.
	// 'record' must be null or inside 'submission'
	// 'cmd' must be empty or a valid hierarchy inside 'record'
	void select(std::vector<FrameSubmission> frame,
		u32 submissionID,
		IntrusivePtr<CommandRecord> record,
		std::vector<const Command*> cmd);

	// Sets updateMode to 'commandBuffer'
	// 'cmd' must be empty or a valid hierarchy inside record
	void select(CommandBufferPtr cb,
		IntrusivePtr<CommandRecord> record,
		std::vector<const Command*> cmd);

	// LocalCapture mode
	void select(const LocalCapture& lc);

	// Only some update modes are valid:
	// - when a whole frame is set, only UpdateMode::swapchain is valid
	// - when a CommandBuffer is set, 'none' or 'commandBuffer' are valid
	// - when a Record without cb is set, 'none' or 'any' are valid
	//   When only a record is set and UpdateMode::commandBuffer is selected here,
	//   will automatically reference it.
	void updateMode(UpdateMode newUpdateMode);

	// Resets the selected CommandHookState.
	// Useful e.g. when hookOps have changed.
	void clearState();

	UpdateMode updateMode() const { return mode_; }
	SelectionType selectionType() const;
	IntrusivePtr<CommandHookState> completedHookState() const { return state_; }

	// Returns null when selectType is not 'command'
	span<const Command* const> command() const { return command_; }
	// Returns null when in 'swapchain' mode and selectionType
	// is not 'record' or 'command'
	IntrusivePtr<CommandRecord> record() const { return record_; }
	// Returns null when not in 'swapchain' mode or when selectionType
	// is 'none'. Points inside frame()
	FrameSubmission* submission() const { return submission_; }
	// Returns null when not in 'swapchain' mode
	span<const FrameSubmission> frame() const { return frame_; }
	// Only for swapchain mode.
	// Returns the present id of the current completed hook state.
	u32 hookStateSwapchainPresent() const { return swapchainPresent_; }
	const auto& descriptorSnapshot() const { return descriptors_; }
	CommandBuffer* cb() const { return cb_.get(); }
	CommandBufferPtr cbPtr() const { return cb_; }

	void unselect();

private:
	void updateHookTarget();

private:
	Device* dev_ {};

	UpdateMode mode_ {};
	IntrusivePtr<CommandHookState> state_; // the last received state
	CommandDescriptorSnapshot descriptors_; // last snapshotted descriptors

	// The currently selected record.
	// In swapchain mode: part of selectedBatch_
	IntrusivePtr<CommandRecord> record_ {};
	// The selected command (hierarchy) inside selectedRecord_.
	// Might be empty, signalling that no command is secleted.
	std::vector<const Command*> command_ {};

	// [swapchain mode]
	// Potentially old, selected command, in its record and its batch.
	// Needed for matching potential candidates later.
	std::vector<FrameSubmission> frame_;
	// Part of frame_
	FrameSubmission* submission_ {};
	u32 swapchainPresent_ {}; // present id of last time we got a completed hook

	// [commandBuffer mode]
	CommandBufferPtr cb_ {};

	const LocalCapture* localCapture_ {};

	LinAllocator matchAlloc_;
};

} // namespace vil
