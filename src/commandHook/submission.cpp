#include <commandHook/submission.hpp>
#include <commandHook/record.hpp>
#include <commandHook/hook.hpp>
#include <command/commands.hpp>
#include <queue.hpp>
#include <device.hpp>
#include <accelStruct.hpp>

namespace vil {

CommandHookSubmission::CommandHookSubmission(CommandHookRecord& rec,
	Submission& subm, CommandDescriptorSnapshot descriptors) :
		record(&rec), descriptorSnapshot(std::move(descriptors)) {
	dlg_assert(rec.state || !rec.accelStructBuilds.empty());
	dlg_assert(!rec.writer);
	rec.writer = &subm;
}

CommandHookSubmission::~CommandHookSubmission() {
	// it's important we have this here (as opposed to in this->finish)
	// for vkQueueSubmit failure cases.
	if(record) {
		dlg_assert(record->record);
		record->writer = nullptr;
	}
}

void CommandHookSubmission::finish(Submission& subm) {
	ZoneScoped;
	dlg_assert(record->writer == &subm);

	// In this case the hook was removed, no longer interested in results.
	// Since we are the only submission left to the record, it can be
	// destroyed.
	if(!record->hook) {
		record->writer = nullptr;
		dlg_assert(!contains(record->record->hookRecords, record));
		delete record;

		// unset for our destructor
		record = nullptr;
		return;
	}

	finishAccelStructBuilds();

	// when the record has no state, we don't have to transmit anything
	if(!record->state) {
		dlg_assert(record->hcommand.empty());
		return;
	}

	transmitTiming();

	auto& state = record->hook->completed.emplace_back();
	state.record = IntrusivePtr<CommandRecord>(record->record);
	state.match = record->match;
	state.state = record->state;
	state.command = record->hcommand;
	state.descriptorSnapshot = std::move(this->descriptorSnapshot);
	state.submissionID = subm.parent->globalSubmitID;

	// This usually is a sign of a problem somewhere inside the layer.
	// Either we are not correctly clearing completed states from the gui
	// but still producing new ones or we have just *waaay* to many
	// candidates and should somehow improve matching for this case.
	dlg_assertlm(dlg_level_warn, record->hook->completed.size() < 64,
		"High number of hook states detected");

	// indirect command readback
	if(record->hook->copyIndirectCmd) {
		auto& bcmd = *record->hcommand.back();

		if(auto* cmd = dynamic_cast<const DrawIndirectCountCmd*>(&bcmd)) {
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
		} else if(auto* cmd = dynamic_cast<const DrawIndirectCmd*>(&bcmd)) {
			[[maybe_unused]] auto cmdSize = cmd->indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			dlg_assert(record->state->indirectCopy.size == cmd->drawCount * cmdSize);

			record->state->indirectCommandCount = cmd->drawCount;
			record->state->indirectCopy.invalidateMap();
		} else if(dynamic_cast<const DispatchIndirectCmd*>(&bcmd)) {
			dlg_assert(record->state->indirectCopy.size == sizeof(VkDispatchIndirectCommand));

			record->state->indirectCommandCount = 1u;
			record->state->indirectCopy.invalidateMap();
		} else if(dynamic_cast<const TraceRaysIndirectCmd*>(&bcmd)) {
			dlg_assert(record->state->indirectCopy.size == sizeof(VkTraceRaysIndirectCommandKHR));

			record->state->indirectCommandCount = 1u;
			record->state->indirectCopy.invalidateMap();
		} else {
			dlg_warn("Unsupported indirect command (readback)");
		}
	}
}

void CommandHookSubmission::transmitTiming() {
	ZoneScoped;

	auto& dev = *record->record->dev;
	if(!record->hook->queryTime) {
		return;
	}

	if(!record->queryPool) {
		// The query pool couldn't be created.
		// This could be the case when the queue does not support
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
	for(auto& buildCmd : record->accelStructBuilds) {
		for(auto& build : buildCmd.builds) {
			dlg_assert(build.dst);

			auto& dst = *build.dst;
			dlg_assert(build.rangeInfos.size() == build.info.geometryCount);
			dlg_assert(build.dst->geometryType != VK_GEOMETRY_TYPE_MAX_ENUM_KHR);

			// we only need this additional copy/retrieve step when a top
			// level accelStruct (with instances) was built, otherwise
			// we already copied everything into the right position.
			if(build.dst->geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
				copyBuildData(dst, build.info, build.rangeInfos.data(), true);
			}
		}
	}
}

} // namespace vil

