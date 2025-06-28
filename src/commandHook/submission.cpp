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

		if(record->invalid) {
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
			auto& hook = record->commandHook();
			dstCapture.blases = captureBLASesLocked(*hook.dev_);
		}
	}
}

void CommandHookSubmission::finish(Submission& subm) {
	ZoneScoped;
	dlg_assert(record->writer == &subm);

	// In this case the hook was invalidated, no longer interested in results.
	// Since we are the only submission left to the record, it can be
	// destroyed.
	if(record->invalid) {
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

	// === debug ====
	/*
	if(record->shaderTable.buf) {
		dlg_assert(record->shaderCapture);

		dlg_trace("shaderTable content");
		auto data = record->shaderTable.data();

		while(data.size() >= 64u) {
			std::string str;
			for(auto i = 0u; i < 16; ++i) {
				str += dlg::format("{}{} ", std::hex, read<u32>(data));
			}

			dlg_trace(" >> {}", str);
		}
	}
	*/
	// === /debug ====

	assertOwned(record->record->dev->mutex);
	transmitTiming();

	// This usually is a sign of a problem somewhere inside the layer.
	// Either we are not correctly clearing completed states from the gui
	// but still producing new ones or we have just *waaay* to many
	// candidates and should somehow improve matching for this case.
	auto& hook = record->commandHook();
	dlg_assertlm(dlg_level_warn, hook.completed_.size() < 64,
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

	// update vertex hints
	auto& hints = hook.hintsLocked();
	auto& ops = hook.opsLocked();
	if(!record->localCapture && (ops.copyVertexInput || ops.copyXfb)) {
		auto& bcmd = *record->hcommand.back();

		ReadBuf span {};
		bool indexed {};
		u32 count {};
		if(auto* cmd = commandCast<const DrawIndirectCountCmd*>(&bcmd)) {
			auto& ic = record->state->indirectCopy;
			span = ic.data();
			count = read<u32>(span); // skip in span
			indexed = cmd->isIndexed();
		} else if(auto* cmd = commandCast<const DrawIndirectCmd*>(&bcmd)) {
			auto& ic = record->state->indirectCopy;
			span = ic.data();
			indexed = cmd->isIndexed();
			count = cmd->drawCount;
		}

		if(!span.empty()) {
			auto changed = false;

			if(ops.copyXfb) {
				// for xfb, we have to copy everything *up to* the selected command
				u32 vertexCount = 0u;
				u32 sumCount = (ops.vertexCmd == u32(-1)) ? count : ops.vertexCmd + 1;
				for(auto i = 0u; i < std::min(sumCount, count); ++i) {
					if(indexed) {
						auto cmd = read<VkDrawIndexedIndirectCommand>(span);
						vertexCount += cmd.indexCount * cmd.instanceCount;
					} else {
						auto cmd = read<VkDrawIndirectCommand>(span);
						vertexCount += cmd.vertexCount * cmd.instanceCount;
					}
				}

				// NOTE: for xfb hints, we just always use vertexCountHint
				changed |= max(hints.vertexCountHint, vertexCount);
			} else {
				if(indexed) {
					skip(span, sizeof(VkDrawIndexedIndirectCommand) * ops.vertexCmd);
					auto cmd = read<VkDrawIndexedIndirectCommand>(span);
					changed |= max(hints.indexCountHint, cmd.indexCount);
					changed |= max(hints.instanceCountHint, cmd.instanceCount);
				} else {
					skip(span, sizeof(VkDrawIndirectCommand) * ops.vertexCmd);
					auto cmd = read<VkDrawIndirectCommand>(span);
					changed |= max(hints.vertexCountHint, cmd.vertexCount);
					changed |= max(hints.instanceCountHint, cmd.instanceCount);
				}
			}

			// make sure to hook new records with better hints
			if(changed) {
				dlg_info("increasing hint size; invalidating records");

				hook.invalidateRecordingsLocked();
				// TODO: just invalidate instead?
				hook.keepAliveLC_.insert(hook.keepAliveLC_.end(),
					std::make_move_iterator(hook.completed_.begin()),
					std::make_move_iterator(hook.completed_.end()));
				hook.completed_.clear();

				// important that we don't insert this state into the
				// list of completed submissions
				return;
			}
		}
	}

	CompletedHook* dstCompleted {};
	if(record->localCapture) {
		if(record->localCapture->flags & LocalCaptureBits::once) {
			dlg_assert(!record->localCapture->completed.state);

			auto& lcs = hook.localCaptures_;
			auto finder = [&](auto& ptr){ return ptr.get() == record->localCapture; };
			auto it = find_if(lcs, finder);
			dlg_assert(it != lcs.end());
			auto ptr = std::move(*it);
			lcs.erase(it);
			hook.localCapturesCompleted_.push_back(std::move(ptr));

			dlg_trace("completed local capture (first) '{}'", record->localCapture->name);
		}

		if(record->localCapture->completed.state) {
			// TODO: hacky af. Needed because we can't destroy the record
			// here (intrusivePtr) since the device mutex is locked.
			// Maybe just change that?
			dlg_assert(record->localCapture->completed.record);
			hook.keepAliveLC_.push_back(std::move(record->localCapture->completed));

			dlg_trace("updating local capture state '{}'", record->localCapture->name);
		}

		dstCompleted = &record->localCapture->completed;
	} else {
		dstCompleted = &hook.completed_.emplace_back();
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

	// debug timing
#ifdef VIL_DEBUG
	auto timingCounts = record->ownTimingNames.size();
	if(timingCounts && dev.printVertexCaptureTimings) {
		u64 data[CommandHookRecord::maxDebugTimings];
		dlg_assert(timingCounts <= CommandHookRecord::maxDebugTimings);
		res = dev.dispatch.GetQueryPoolResults(dev.handle, record->queryPool,
			2, timingCounts, sizeof(data), data, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
		if(res != VK_SUCCESS) {
			dlg_error("GetQueryPoolResults failed: {}", res);
			return;
		}

		auto last = data[0];
		dlg_trace("== debug timings ==");
		for(auto [i, name] : enumerate(record->ownTimingNames)) {
			if(i == 0u) {
				continue;
			}

			auto diff = data[i] - last;
			auto diffMS = dev.props.limits.timestampPeriod * diff / (1000.f * 1000.f);
			dlg_trace("  {}: {} ms", name, diffMS);

			last = data[i];
		}
	}
#endif // VIL_DEBUG
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

