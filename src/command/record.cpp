#include <command/record.hpp>
#include <command/commands.hpp>
#include <command/alloc.hpp>
#include <stats.hpp>
#include <image.hpp>
#include <pipe.hpp>
#include <cb.hpp>
#include <ds.hpp>
#include <util/util.hpp>
#include <gui/commandHook.hpp>

namespace vil {

void onRecordAlloc(const std::byte* buf, u32 size) {
	(void) buf;
	(void) size;
	TracyAllocS(buf, size, 8);
	DebugStats::get().commandMem += size;
}

void onRecordFree(const std::byte* buf, u32 size) {
	(void) buf;
	(void) size;
	TracyFreeS(buf, 8);
	DebugStats::get().commandMem -= size;
}

// Record
CommandRecord::CommandRecord(CommandBuffer& xcb) :
		alloc(onRecordAlloc, onRecordFree),
		dev(xcb.dev),
		cb(&xcb),
		recordID(xcb.recordCount()),
		queueFamily(xcb.pool().queueFamily),
		// initialize allocators
		pushLables(alloc),
		handles(alloc),
		invalidated(alloc),
		pipeLayouts(alloc),
		dsUpdateTemplates(alloc),
		secondaries(alloc) {
	if(!cb->name.empty()) {
		cbName = copyString(*this, cb->name);
	}

	++DebugStats::get().aliveRecords;
}

CommandRecord::CommandRecord(ManualTag, Device& dev) :
		alloc(onRecordAlloc, onRecordFree),
		dev(&dev),
		cb(nullptr),
		recordID(0u),
		queueFamily(0u),
		// initialize allocators
		pushLables(alloc),
		handles(alloc),
		invalidated(alloc),
		pipeLayouts(alloc),
		dsUpdateTemplates(alloc),
		secondaries(alloc) {
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
	record.commands->replace(record.invalidated);

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
	assertOwned(dev.mutex);

	if(state.pipe) {
		dev.dispatch.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
			state.pipe->handle);
	}

	for(auto i = 0u; i < state.descriptorSets.size(); ++i) {
		auto& bds = state.descriptorSets[i];
		auto [pds, lock] = tryAccessLocked(bds);
		auto& ds = nonNull(pds);

		// NOTE: we only need this since we don't track this during recording
		// anymore at the moment.
		if(state.pipe && !compatibleForSetN(*state.pipe->layout,
				*bds.layout, i)) {
			break;
		}

		dlg_assert(ds.layout);
		dev.dispatch.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
			bds.layout->handle, i, 1u, &ds.handle,
			u32(bds.dynamicOffsets.size()), bds.dynamicOffsets.data());
	}
}

std::pair<DescriptorSet*, std::unique_lock<decltype(DescriptorPool::mutex)>>
tryAccessLocked(const BoundDescriptorSet& bds) {
	if(!bds.dsPool) {
		dlg_debug("DescriptorSet inaccessible; DescriptorSet was destroyed");
		return {};
	}

	// this functions requires the device mutex to be locked to make sure
	// that the descriptor pool can't be destroyed.
	assertOwned(bds.dsPool->dev->mutex);
	auto lock = std::unique_lock(bds.dsPool->mutex);

	auto& entry = *static_cast<DescriptorPoolSetEntry*>(bds.dsEntry);
	if(!entry.set) {
		dlg_warn("DescriptorSet inaccessible; DescriptorSet was destroyed");
		return {};
	}

	auto& ds = *entry.set;
	dlg_assert(reinterpret_cast<std::byte*>(&ds) - bds.dsPool->data.get() < bds.dsPool->dataSize);
	if(ds.id != bds.dsID) {
		dlg_warn("DescriptorSet inaccessible; DescriptorSet was destroyed (overwritten)");
		return {};
	}

	return {&ds, std::move(lock)};
}

DescriptorSet& access(const BoundDescriptorSet& bds) {
	dlg_assert(bds.dsPool);

	auto& entry = *static_cast<DescriptorPoolSetEntry*>(bds.dsEntry);
	dlg_assert(entry.set);

	auto& ds = *entry.set;
	dlg_assert(reinterpret_cast<std::byte*>(&ds) - bds.dsPool->data.get() < bds.dsPool->dataSize);
	dlg_assert(ds.id == bds.dsID);

	return ds;
}

CommandDescriptorSnapshot snapshotRelevantDescriptorsLocked(const Command& cmd) {
	// assertOwned(dev.mutex);

	CommandDescriptorSnapshot ret;
	auto* scmd = dynamic_cast<const StateCmdBase*>(&cmd);
	if(!scmd) {
		return ret;
	}

	for(auto bds : scmd->boundDescriptors().descriptorSets) {
		auto [ds, lock] = tryAccessLocked(bds);
		if(ds) {
			ret.states.emplace(bds.dsEntry, addCow(*ds));
		}
	}

	return ret;
}

bool potentiallyWrites(const CommandRecord& rec, const Image& img) {
	auto mi = const_cast<DeviceHandle*>(static_cast<const DeviceHandle*>(&img));
	auto it = rec.handles.find(mi);
	if(it == rec.handles.end()) {
		return false;
	}

	auto& usedImage = static_cast<UsedImage&>(*it->second);
	return usedImage.potentiallyWrites;
}

} // namespace vil
