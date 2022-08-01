#include <command/record.hpp>
#include <command/commands.hpp>
#include <command/alloc.hpp>
#include <commandHook/record.hpp>
#include <stats.hpp>
#include <image.hpp>
#include <pipe.hpp>
#include <cb.hpp>
#include <ds.hpp>
#include <util/util.hpp>

// for used handles
#include <sync.hpp>
#include <queryPool.hpp>
#include <buffer.hpp>
#include <image.hpp>
#include <rp.hpp>
#include <accelStruct.hpp>

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
		used(alloc),
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
		used(alloc),
		secondaries(alloc) {
	++DebugStats::get().aliveRecords;
}

CommandRecord::~CommandRecord() {
	if(!dev) {
		return;
	}

	ZoneScoped;

	{
		// HookRecord destructor might reference this.
		// And it must be called while mutex is locked.
		// TODO: don't require that
		std::lock_guard lock(dev->mutex);
		ZoneScopedN("clear hookRecords");
		hookRecords.clear();
	}

	dlg_assert(DebugStats::get().aliveRecords > 0);
	--DebugStats::get().aliveRecords;
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
		auto [ds, lock] = tryAccessLocked(bds);
		dlg_assert(ds);

		// NOTE: we only need this since we don't track this during recording
		// anymore at the moment.
		if(state.pipe && !compatibleForSetN(*state.pipe->layout,
				*bds.layout, i)) {
			break;
		}

		dlg_assert(ds->layout);
		dev.dispatch.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
			bds.layout->handle, i, 1u, &ds->handle,
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
			ret.states.emplace(bds.dsEntry, ds->addCow());
		}
	}

	return ret;
}

bool findHierarchy(std::vector<const Command*>& hierarchy,
		const Command& parent, const Command& dst) {
	dlg_assert(hierarchy.empty());

	auto it = parent.children();
	while(it) {
		if(it == &dst || findHierarchy(hierarchy, *it, dst)) {
			hierarchy.push_back(it);
			return true;
		}

		it = it->next;
	}

	return false;
}

std::vector<const Command*> findHierarchy(const CommandRecord& rec, const Command& dst) {
	std::vector<const Command*> ret;
	auto success = findHierarchy(ret, *rec.commands, dst);
	dlg_assert(success != (ret.empty()));

	// don't append root command
	// need to reverse here, constructing in reverse order is more
	// efficient during recursive traversal above
	std::reverse(ret.begin(), ret.end());

	return ret;
}

CommandRecord::UsedHandles::UsedHandles(LinAllocator& alloc) :
		buffers(alloc),
		graphicsPipes(alloc),
		computePipes(alloc),
		rtPipes(alloc),
		pipeLayouts(alloc),
		dsuTemplates(alloc),
		renderPasses(alloc),
		framebuffers(alloc),
		queryPools(alloc),
		imageViews(alloc),
		bufferViews(alloc),
		samplers(alloc),
		accelStructs(alloc),
		events(alloc),
		descriptorSets(alloc),
		images(alloc) {
}

} // namespace vil
