#include <gui/commandSelection.hpp>
#include <commandHook/hook.hpp>
#include <commandHook/state.hpp>
#include <command/match.hpp>
#include <ds.hpp>
#include <memory.hpp>
#include <image.hpp>
#include <buffer.hpp>
#include <queue.hpp>
#include <device.hpp>
#include <swapchain.hpp>

namespace vil {

CommandSelection::CommandSelection() = default;
CommandSelection::~CommandSelection() = default;

bool CommandSelection::update() {
	auto& dev = *dev_;
	auto& hook = *dev.commandHook;
	auto completed = hook.moveCompleted();

	if(mode_ == UpdateMode::localCapture) {
		dlg_assert(localCapture_);

		if(hook.freeze.load()) {
			// NOTE: even if there is a new state, don't update.
			//   LocalCaptures are updated independently from the freeze
			//   flag since we never want to miss them.
			return false;
		}

		IntrusivePtr<CommandRecord> record;
		IntrusivePtr<CommandHookState> state;
		CommandDescriptorSnapshot descriptors;

		{
			std::lock_guard lock(dev_->mutex);

			// check if the LocalCapture has found something new.
			if(localCapture_->completed.state == state_) {
				return false;
			}

			record = localCapture_->completed.record;
			command_ = localCapture_->completed.command;
			state = localCapture_->completed.state;
			descriptors = localCapture_->completed.descriptorSnapshot;
		}

		state_ = std::move(state);
		record_ = std::move(record);
		descriptors_ = std::move(descriptors);
		return true;
	}

	// TODO: we want the second condition (maybe assert that completed is
	//   empty when freezeState is true) but atm that means we would not
	//   update state on hook ops change. See todo
	if(completed.empty() /*|| (freezeState && hook.freeze.load() && state_)*/) {
		// nothing to do
		return false;
	}

	dlg_assert(selectionType() != SelectionType::none);

	// find the best match
	CompletedHook* best = nullptr;
	std::vector<FrameSubmission> bestBatches; // only for swapchain mode
	ThreadMemScope tms;
	u32 bestPresentID = {};

	auto swapchain = dev.swapchain();

	auto frameForSubmissionLocked = [&](u32 submissionID) -> const FrameSubmissions* {
		assertOwned(dev.mutex);
		if(!swapchain) {
			dlg_warn("lost swapchain");
			return nullptr;
		}

		for(auto& frame : swapchain->frameSubmissions) {
			if(submissionID >= frame.submissionStart &&
					submissionID <= frame.submissionEnd) {
				return &frame;
			}
		}

		dlg_warn("Couldn't find frame associated to hooked submission");
		return nullptr;
	};

	// TODO: just set this to true for non-swapchain modes as well?
	bool chooseLast = false;
	if(mode_ == UpdateMode::swapchain) {
		bool multipleMatchesInLastFrame = false;
		if(completed.size() > 1u) {
			auto id1 = completed[completed.size() - 1].submissionID;
			auto id2 = completed[completed.size() - 2].submissionID;

			std::lock_guard lock(dev.mutex);
			auto* frame1 = frameForSubmissionLocked(id1);
			auto* frame2 = frameForSubmissionLocked(id2);
			multipleMatchesInLastFrame = frame1 && (frame1 == frame2);
		}

		chooseLast = !multipleMatchesInLastFrame;
	}

	auto bestMatch = 0.f;
	for(auto& res : completed) {
		if(res.match <= bestMatch) {
			continue;
		}

		if(mode_ == UpdateMode::swapchain) {
			// NOTE: before, we did a complete frame-matching here again,
			// even though it's expensive. Might improve precision a bit
			// compared to the prefix-only matching we do in CommandHook.
			// Re-introduce this here again if ever needed, see design.md

			[[maybe_unused]] auto selType = selectionType();
			dlg_assert(selType == SelectionType::command ||
				selType == SelectionType::record ||
				selType == SelectionType::submission);

			{
				std::lock_guard lock(dev.mutex);
				auto* frame = frameForSubmissionLocked(res.submissionID);
				if(!frame) {
					continue;
				}

				bestBatches = frame->batches;
				bestPresentID = frame->presentID;
			}

		}

		best = &res;
		bestMatch = res.match;

		if(chooseLast) {
			break;
		}
	}

	if(!best) {
		return false;
	}

	// Update internal state state from hook match
	// In swapchain mode - and when not freezing commands - make
	// sure to also display the new frame
	if(mode_ == UpdateMode::swapchain) {
		swapchainPresent_ = bestPresentID;
		frame_ = std::move(bestBatches);

		submission_ = nullptr;
		for(auto& subm : frame_) {
			if(subm.submissionID == best->submissionID) {
				submission_ = &subm;
				break;
			}
		}
		dlg_assert(submission_);
	}

	record_ = best->record;

	// NOTE: somewhat hacky, the way we handle whole-record selections.
	if(best->command.size() == 1u) {
		dlg_assert(command_.empty());
	} else {
		command_ = best->command;
	}

	state_ = best->state;
	descriptors_ = best->descriptorSnapshot;

	// update the hook
	updateHookTarget();

	// in this case, we want to freeze state but temporarily
	// unfroze it to get a new CommandHookState. This happens
	// e.g. when selecting a new command or viewed resource
	if(!hook.freeze && freezeState) {
		hook.freeze.store(true);
	}

	return true;
}

void CommandSelection::select(IntrusivePtr<CommandRecord> record,
		std::vector<const Command*> cmd) {
	unselect();

	mode_ = UpdateMode::none;
	record_ = std::move(record);
	command_ = std::move(cmd);

	if(!command_.empty()) {
		descriptors_ = snapshotRelevantDescriptors(*dev_, *command_.back());
	}

	updateHookTarget();
}

void CommandSelection::select(std::vector<FrameSubmission> frame,
		u32 submissionID,
		IntrusivePtr<CommandRecord> record,
		std::vector<const Command*> cmd) {
	unselect();

	mode_ = UpdateMode::swapchain;
	frame_ = std::move(frame);

	// TODO: HACK, see todo on multi-swapchain support
	auto swapchain = dev_->swapchain();
	dlg_assertl(dlg_level_warn, swapchain); // only warning cause our logic is racy
	if(swapchain) {
		swapchainPresent_ = swapchain->presentCounter;
	}

	if(submissionID == u32(-1)) {
		dlg_assert(cmd.empty());
		dlg_assert(!record);
		submission_ = nullptr;
	} else {
		dlg_assert(submissionID < frame_.size());
		submission_ = &frame_[submissionID];
	}

	command_ = std::move(cmd);
	record_ = std::move(record);

	if(!command_.empty()) {
		descriptors_ = snapshotRelevantDescriptors(*dev_, *command_.back());
	}

	if(submission_) {
		dlg_assertm(submission_ >= frame_.data() &&
			submission_ < frame_.data() + frame_.size(),
			"'submission' must be inside 'frame'");
	}

	updateHookTarget();
}

void CommandSelection::select(CommandBufferPtr cb,
		IntrusivePtr<CommandRecord> record,
		std::vector<const Command*> cmd) {
	unselect();
	dlg_assert(cb);
	dlg_assert(record);
	dlg_assert(!record->cb || record->cb == cb.get());

	mode_ = UpdateMode::commandBuffer;
	cb_ = std::move(cb);
	record_ = std::move(record);
	command_ = std::move(cmd);

	if(!command_.empty()) {
		descriptors_ = snapshotRelevantDescriptors(*dev_, *command_.back());
	}

	updateHookTarget();
}

void CommandSelection::select(const LocalCapture& lc) {
	unselect();

	mode_ = UpdateMode::localCapture;

	IntrusivePtr<CommandRecord> record;
	IntrusivePtr<CommandHookState> state;

	{
		std::lock_guard lock(dev_->mutex);
		record = lc.record;
		state = lc.completed.state;
		command_ = lc.command;
	}

	state_ = std::move(state);
	record_ = std::move(record);
	localCapture_ = &lc;

	if(!command_.empty()) {
		descriptors_ = snapshotRelevantDescriptors(*dev_, *command_.back());
	}

	updateHookTarget();
}

void CommandSelection::unselect() {
	auto& hook = *dev_->commandHook;

	CommandHook::Update update;
	update.invalidate = true;
	update.newTarget = CommandHook::Target {};
	update.newOps = CommandHook::Ops {};

	hook.updateHook(std::move(update));

	cb_ = {};
	state_ = {};
	frame_ = {};
	record_ = {};
	command_ = {};
	submission_ = {};
	descriptors_ = {};
	swapchainPresent_ = 0u;

	mode_ = UpdateMode::none;
}

void CommandSelection::updateHookTarget() {
	CommandHook::Update update;

	auto& target = update.newTarget.emplace();
	target.cb = cb_;
	target.command = command_;
	target.record = record_;
	target.descriptors = descriptors_;

	switch(mode_) {
		case UpdateMode::any:
			target.type = CommandHook::TargetType::all;
			break;
		case UpdateMode::commandBuffer:
			target.type = CommandHook::TargetType::commandBuffer;
			break;
		case UpdateMode::swapchain:
			if(!submission_) {
				dlg_assert(!record_);
				dlg_assert(command_.empty());
				target.type = CommandHook::TargetType::none;
				break;
			}

			target.submissionID = u32(submission_ - frame_.data());
			target.frame = frame_;
			target.type = CommandHook::TargetType::inFrame;
			break;
		case UpdateMode::none:
			target.type = CommandHook::TargetType::commandRecord;
			break;
		case UpdateMode::localCapture:
			target.type = CommandHook::TargetType::none;
			break;
	}

	dev_->commandHook->updateHook(std::move(update));
}

void CommandSelection::updateMode(UpdateMode newUpdateMode) {
	mode_ = newUpdateMode;
	updateHookTarget();
}

CommandSelection::SelectionType CommandSelection::selectionType() const {
	if(!command_.empty()) {
		dlg_assert(record_);
		dlg_assert(mode_ != UpdateMode::swapchain ||
			(!frame_.empty() && submission_));
		return SelectionType::command;
	}

	if(record_) {
		dlg_assert(mode_ != UpdateMode::swapchain ||
			(!frame_.empty() && submission_));
		return SelectionType::record;
	}

	if(submission_) {
		dlg_assert(mode_ == UpdateMode::swapchain);
		return SelectionType::submission;
	}

	return SelectionType::none;
}

void CommandSelection::clearState() {
	state_.reset();
}

} // namespace vil
