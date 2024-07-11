#include <commandHook/submission.hpp>
#include <commandHook/record.hpp>
#include <commandHook/hook.hpp>
#include <command/commands.hpp>
#include <queue.hpp>
#include <memory.hpp>
#include <device.hpp>
#include <ds.hpp>
#include <accelStruct.hpp>

namespace vil {

CommandHookSubmission::CommandHookSubmission(CommandHookRecord& rec,
	Submission& subm, CommandDescriptorSnapshot descriptors) :
		record(&rec), descriptorSnapshot(std::move(descriptors)) {
	dlg_assert(!rec.writer);
	rec.writer = &subm;
}

CommandHookSubmission::~CommandHookSubmission() {
	// it's important we have this here (as opposed to in this->finish)
	// for vkQueueSubmit failure cases.
	if(record) {
		dlg_assert(record->record);
		record->writer = nullptr;

		// hook was invalidated, record should be deleted
		if(!record->hook) {
			record->writer = nullptr;
			dlg_assert(!contains(record->record->hookRecords, record));
			delete record;
		}
	}
}

void CommandHookSubmission::activate() {
	for(auto& op : record->accelStructOps) {
		if(auto* buildOp = std::get_if<CommandHookRecord::AccelStructBuild>(&op); buildOp) {
			for(auto& build : buildOp->builds) {
				dlg_assert(build.dst);
				build.dst->pendingState = build.state;
			}
		} else if(auto* copy = std::get_if<CommandHookRecord::AccelStructCopy>(&op); copy) {
			dlg_assert(copy->src->pendingState);
			copy->state = copy->src->pendingState;
			copy->dst->pendingState = copy->src->pendingState;
		} else if(auto* capture = std::get_if<CommandHookRecord::AccelStructCapture>(&op); capture) {
			dlg_assert(record->state);

			auto& dst = record->state->copiedDescriptors[capture->id];
			auto& dstCapture = std::get<CommandHookState::CapturedAccelStruct>(dst.data);

			dlg_assert(capture->accelStruct->pendingState);
			dstCapture.tlas = capture->accelStruct->pendingState;

			// NOTE: this can be quite expensive, in the case of many BLASes
			// This cannot really be optimized though. At this point in time we might
			// not know the current TLAS instances and we do not want to capture the BLASes
			// at any later time since they might have been invalidated then already.
			dstCapture.blases = captureBLASesLocked(*record->hook->dev_);
		}
	}
}

void CommandHookSubmission::finish(Submission& subm) {
	ZoneScoped;
	dlg_assert(record->writer == &subm);

	// In this case the hook was invalidated, no longer interested in results.
	// Since we are the only submission left to the record, it can be
	// destroyed.
	if(!record->hook) {
		finishAccelStructBuilds();

		record->writer = nullptr;
		dlg_assert(!contains(record->record->hookRecords, record));
		delete record;

		// unset for our destructor
		record = nullptr;
		return;
	}

	// when the record has no state, we don't have to transmit anything
	if(!record->state) {
		dlg_assert(record->hcommand.empty());
		finishAccelStructBuilds();
		return;
	}

	assertOwned(record->hook->dev_->mutex);
	transmitTiming();

	// This usually is a sign of a problem somewhere inside the layer.
	// Either we are not correctly clearing completed states from the gui
	// but still producing new ones or we have just *waaay* to many
	// candidates and should somehow improve matching for this case.
	dlg_assertlm(dlg_level_warn, record->hook->completed_.size() < 64,
		"High number of hook states detected");

	// indirect command readback
	if(record->state->indirectCopy.buf) {
		auto& bcmd = *record->hcommand.back();

		if(auto* cmd = commandCast<const DrawIndirectCountCmd*>(&bcmd)) {
			dlg_assert(record->state->indirectCopy.size >= 4u);
			auto* count = reinterpret_cast<const u32*>(record->state->indirectCopy.map);
			record->state->indirectCommandCount = *count;
			dlg_assert(record->state->indirectCommandCount <= cmd->maxDrawCount);

			auto cmdSize = cmd->indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			dlg_assertlm(dlg_level_warn,
				record->state->indirectCopy.size >= 4 + *count * cmdSize,
				"Indirect command readback buffer too small; commands missing");

			// auto cmdsSize = cmdSize * record->state->indirectCommandCount;
			// record->state->indirectCopy.cpuCopy(4u, cmdsSize);
			// record->state->indirectCopy.copyOffset = 0u;
			record->state->indirectCopy.invalidateMap();
		} else if(auto* cmd = commandCast<const DrawIndirectCmd*>(&bcmd)) {
			[[maybe_unused]] auto cmdSize = cmd->indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			dlg_assert(record->state->indirectCopy.size == cmd->drawCount * cmdSize);

			record->state->indirectCommandCount = cmd->drawCount;
			record->state->indirectCopy.invalidateMap();
		} else if(commandCast<const DispatchIndirectCmd*>(&bcmd)) {
			dlg_assert(record->state->indirectCopy.size == sizeof(VkDispatchIndirectCommand));

			record->state->indirectCommandCount = 1u;
			record->state->indirectCopy.invalidateMap();
		} else if(commandCast<const TraceRaysIndirectCmd*>(&bcmd)) {
			dlg_assert(record->state->indirectCopy.size == sizeof(VkTraceRaysIndirectCommandKHR));

			record->state->indirectCommandCount = 1u;
			record->state->indirectCopy.invalidateMap();
		} else {
			dlg_warn("Unsupported indirect command (readback)");
		}
	}

	finishAccelStructBuilds();

	CompletedHook* dstCompleted {};
	if(record->localCapture) {
		if(record->localCapture->flags & LocalCaptureBits::once) {
			dlg_assert(!record->localCapture->completed.state);

			auto& lcs = record->hook->localCaptures_;
			auto finder = [&](auto& ptr){ return ptr.get() == record->localCapture; };
			auto it = find_if(lcs, finder);
			dlg_assert(it != lcs.end());
			auto ptr = std::move(*it);
			lcs.erase(it);
			record->hook->localCapturesCompleted_.push_back(std::move(ptr));

			dlg_trace("completed local capture (first) '{}'", record->localCapture->name);
		}

		if(record->localCapture->completed.state) {
			// TODO: hacky af. Needed because we can't destroy the record
			// here (intrusivePtr) since the device mutex is locked.
			// Maybe just change that?
			dlg_assert(record->localCapture->completed.record);
			record->hook->keepAliveLC_.push_back(std::move(record->localCapture->completed));

			dlg_trace("updating local capture state '{}'", record->localCapture->name);
		}

		dstCompleted = &record->localCapture->completed;
	} else {
		dstCompleted = &record->hook->completed_.emplace_back();
	}

	dstCompleted->record = IntrusivePtr<CommandRecord>(record->record);
	dstCompleted->match = record->match;
	dstCompleted->state = record->state;
	dstCompleted->command = record->hcommand;
	dstCompleted->descriptorSnapshot = std::move(this->descriptorSnapshot);
	dstCompleted->submissionID = subm.parent->globalSubmitID;
}

void CommandHookSubmission::transmitTiming() {
	ZoneScoped;

	auto& dev = *record->record->dev;
	if(!record->queryPool) {
		// We didn't query the time or the query pool couldn't be created.
		// Latter could be the case when the queue does not support
		// timing queries. Signal it.
		record->state->neededTime = u64(-1);
		return;
	}

	// Store the query pool results.
	// Since the submission finished, we can expect them to be available
	// soon, so we wait for them.
	u64 data[2];
	auto res = dev.dispatch.GetQueryPoolResults(dev.handle, record->queryPool, 0, 2,
		sizeof(data), data, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

	// check if query is available
	if(res != VK_SUCCESS) {
		dlg_error("GetQueryPoolResults failed: {}", res);
		return;
	}

	u64 before = data[0];
	u64 after = data[1];

	auto diff = after - before;
	record->state->neededTime = diff;
}

void CommandHookSubmission::finishAccelStructBuilds() {
	// Notify all accel struct builds that they have finished.
	// We are guaranteed by the standard that all accelStructs build
	// by the submission are still valid at this point.
	for(auto& op : record->accelStructOps) {
		if(auto* buildOp = std::get_if<CommandHookRecord::AccelStructBuild>(&op); buildOp) {
			for(auto& build : buildOp->builds) {
				dlg_assert(build.state);
				build.state->built = true;

				// TODO: needed?
				build.state->buffer.invalidateMap();

				build.dst->lastValid = build.state;
			}
		} else if(auto* copy = std::get_if<CommandHookRecord::AccelStructCopy>(&op); copy) {
			dlg_assert(copy->dst->pendingState);
			dlg_assert(copy->state);
			copy->dst->lastValid = copy->state;
		}
	}
}

} // namespace vil

