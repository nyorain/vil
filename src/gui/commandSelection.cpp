#include <gui/commandSelection.hpp>
#include <commandHook/hook.hpp>
#include <commandHook/state.hpp>
#include <command/match.hpp>
#include <ds.hpp>
#include <device.hpp>
#include <swapchain.hpp>

namespace vil {

bool CommandSelection::update() {
	auto& dev = *dev_;
	auto& hook = *dev.commandHook;
	auto completed = hook.moveCompleted();

	if(completed.empty() || (freezeState && state_)) {
		// nothing to do
		return false;
	}

	dlg_assert(selectionType() != SelectionType::none);

	// find the best match
	CommandHook::CompletedHook* best = nullptr;
	auto bestMatch = 0.f;
	ThreadMemScope tms;
	FrameMatch bestMatchResult {};
	u32 bestPresentID = {};
	std::vector<FrameSubmission> bestBatches; // only for swapchain mode

	// TODO PERF we probably don't want/need all that matching logic
	// here anymore, we match now already when hooking.
	// Maybe just choose the last completed hook here?
	// Or only match (and choose the best candidate) when there are multiple
	// candidates in the last frame?
	for(auto& res : completed) {
		float resMatch = res.match;

		// When we are in swapchain mode, we need the frame associated with
		// this submission. We will then also consider the submission in its
		// context inside the frame.
		std::vector<FrameSubmission> frameSubmissions;
		u32 presentID {};

		FrameMatch batchMatches;
		if(mode_ == UpdateMode::swapchain) {
			[[maybe_unused]] auto selType = selectionType();
			dlg_assert(selType == SelectionType::command ||
				selType == SelectionType::record ||
				selType == SelectionType::submission);

			{
				std::lock_guard lock(dev.mutex);
				for(auto& frame : dev.swapchain->frameSubmissions) {
					if(res.submissionID >= frame.submissionStart &&
							res.submissionID <= frame.submissionEnd) {

						frameSubmissions = frame.batches;
						presentID = frame.presentID;
						break;
					}
				}
			}

			// when the hook is from too long ago, we won't
			if(frameSubmissions.empty()) {
				dlg_warn("Couldn't find frame associated to hooked submission");
				continue;
			}

			dlg_assert(!frame_.empty());

			// check if [record_ in frame_] contextually
			// matches  [res.record in foundBatches]
			if(record_) {
				LinAllocScope localMatchMem(matchAlloc_);
				batchMatches = match(tms, localMatchMem, frame_, frameSubmissions);
				bool recordMatched = false;
				for(auto& batchMatch : batchMatches.matches) {
					if(batchMatch.b->submissionID != res.submissionID) {
						continue;
					}

					for(auto& recMatch : batchMatch.matches) {
						if(recMatch.b != res.record) {
							continue;
						}

						if(recMatch.a == record_.get()) {
							recordMatched = true;
							resMatch *= eval(recMatch.match);

							// TODO: For all parent commands, we could make
							// sure that they match.
							// I.e. (command_[i], res.command[i]) somewhere
							// in the recMatch.matches[X]...matches hierarchy
						}

						break;
					}

					break;
				}

				// In this case, the newly hooked potentialy candidate did
				// not match with our previous record in content; we won't use it.
				if(!recordMatched) {
					dlg_info("Hooked record did not match. Total match: {}, match count {}",
						eval(batchMatches.match), batchMatches.matches.size());
					continue;
				}
			}
		}

		if(resMatch > bestMatch) {
			best = &res;
			bestMatch = resMatch;
			bestBatches = frameSubmissions;
			bestPresentID = presentID;
			bestMatchResult = batchMatches;
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
	command_ = best->command;
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

void CommandSelection::unselect() {
	auto& hook = *dev_->commandHook;

	CommandHook::HookUpdate update;
	update.stillNeeded = nullptr;
	update.invalidate = true;
	update.newTarget = CommandHook::HookTarget {};
	update.newOps = CommandHook::HookOps {};

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
	CommandHook::HookUpdate update;
	update.stillNeeded = state_.get();

	auto& target = update.newTarget.emplace();
	target.cb = cb_;
	target.command = command_;
	target.record = record_;
	target.descriptors = descriptors_;

	switch(mode_) {
		case UpdateMode::any:
			target.type = CommandHook::HookTargetType::all;
			break;
		case UpdateMode::commandBuffer:
			target.type = CommandHook::HookTargetType::commandBuffer;
			break;
		case UpdateMode::swapchain:
			if(!submission_) {
				dlg_assert(!record_);
				dlg_assert(command_.empty());
				target.type = CommandHook::HookTargetType::none;
				break;
			}

			target.submissionID = u32(submission_ - frame_.data());
			target.frame = frame_;
			target.type = CommandHook::HookTargetType::inFrame;
			break;
		case UpdateMode::none:
			target.type = CommandHook::HookTargetType::commandRecord;
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

} // namespace vil
