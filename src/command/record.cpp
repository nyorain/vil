#include <command/record.hpp>
#include <command/commands.hpp>
#include <image.hpp>
#include <pipe.hpp>
#include <cb.hpp>
#include <ds.hpp>
#include <util/util.hpp>
#include <gui/commandHook.hpp>

namespace vil {

// Record
CommandRecord::CommandRecord(CommandBuffer& xcb) :
		dev(xcb.dev),
		cb(&xcb),
		recordID(xcb.recordCount()),
		queueFamily(xcb.pool().queueFamily),
		// initialize allocators
		pushLables(*this),
		handles(*this),
		invalidated(*this),
		pipeLayouts(*this),
		dsUpdateTemplates(*this),
		secondaries(*this) {
	if(!cb->name.empty()) {
		cbName = copyString(*this, cb->name);
	}

	++DebugStats::get().aliveRecords;
}

CommandRecord::~CommandRecord() {
	if(!dev) {
		return;
	}

	ZoneScoped;

	{
		std::lock_guard lock(dev->mutex);

		// remove record from all referenced resources
		for(auto& [handle, uh] : handles) {
			if(invalidated.find(handle) != invalidated.end()) {
				continue;
			}

			if(uh->next == uh && uh->prev == uh) {
				// descriptor set, nothing to do
				continue;
			}

			dlg_assert(handle->refRecords);
			if(uh->prev) {
				uh->prev->next = uh->next;
			} else {
				dlg_assert(uh == handle->refRecords);
				handle->refRecords = uh->next;
			}

			if(uh->next) {
				uh->next->prev = uh->prev;
			}
		}

		// Its destructor might reference this.
		// And it must be called while mutex is locked.
		// TODO: don't require that
		hookRecords.clear();

		dlg_assert(DebugStats::get().aliveRecords > 0);
		--DebugStats::get().aliveRecords;
	}
}

void replaceInvalidatedLocked(CommandRecord& record) {
	ZoneScoped;

	if(record.invalidated.empty()) {
		return;
	}

	// unset in commands
	// NOTE: we could query commands where handles are used via usedHandles
	// maps. Might give speedup for large command buffers. But introduces
	// new complexity and problems, maybe not worth it.
	// Same optimization below when removing from usedHandles.
	// Would need the raw vulkan handle though, we don't access to that
	// here anyways at the moment. But that should be doable if really
	// needed, might be good idea to move the usedHandles maps to use
	// our Handles (i.e. Image*) as key anyways.
	auto* cmd = record.commands;
	while(cmd) {
		cmd->replace(record.invalidated);
		cmd = cmd->next;
	}

	// remove from handles
	for(auto it = record.handles.begin(); it != record.handles.end(); ) {
		if(record.invalidated.find(it->first) != record.invalidated.end()) {
			it = record.handles.erase(it);
		} else {
			++it;
		}
	}

	record.invalidated.clear();
}

// util
void bind(Device& dev, VkCommandBuffer cb, const ComputeState& state) {
	if(state.pipe) {
		dev.dispatch.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
			state.pipe->handle);
	}

	for(auto i = 0u; i < state.descriptorSets.size(); ++i) {
		auto& ds = state.descriptorSets[i];
		if(!ds.ds) {
			continue;
		}

		// NOTE: we only need this since we don't track this during recording
		// anymore at the moment.
		if(state.pipe && !compatibleForSetN(*state.pipe->layout,
				*ds.layout, i)) {
			break;
		}

		dlg_assert(ds.layout);
		dev.dispatch.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
			ds.layout->handle, i, 1u, &static_cast<DescriptorSet*>(ds.ds)->handle,
			u32(ds.dynamicOffsets.size()), ds.dynamicOffsets.data());
	}
}

CommandDescriptorSnapshot snapshotRelevantDescriptors(const Command& cmd) {
	CommandDescriptorSnapshot ret;
	auto* scmd = dynamic_cast<const StateCmdBase*>(&cmd);
	if(!scmd) {
		return ret;
	}

	// TODO: add this functionality to BoundDescriptorSet?
	// Can be useful in a couple of other places.
	// Also add it as a check version so we can use it via asserts
	// where appropriate (e.g. bindState).

	for(auto bds : scmd->boundDescriptors().descriptorSets) {
		if(!bds.dsPool) {
			dlg_warn("DescriptorSet inaccessible; DescriptorPool was destroyed");
			continue;
		}

		// TODO: eh kinda sketchy. Maybe instead of ds rather reference
		// the DescriptorPoolSetEntry? That is guaranteed to stay valid.
		// We might access memory that is used for something else here.
		auto& ds = *static_cast<DescriptorSet*>(bds.ds);
		dlg_assert(reinterpret_cast<std::byte*>(bds.ds) - bds.dsPool->data.get() < bds.dsPool->dataSize);
		if(ds.id != bds.dsID) {
			dlg_warn("DescriptorSet inaccessible; DescriptorSet was destroyed");
			continue;
		}

		ret.states.emplace(bds.ds, addCow(ds));
	}

	return ret;
}

} // namespace vil
