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
		CommandRecord(manualTag, xcb.dev) {
	cb = &xcb;
	recordID = xcb.recordCount();
	queueFamily = xcb.pool().queueFamily;

	if(!cb->name.empty()) {
		cbName = copyString(*this, cb->name);
	}
}

CommandRecord::CommandRecord(ManualTag, Device* xdev) :
		alloc(onRecordAlloc, onRecordFree),
		dev(xdev),
		cb(nullptr),
		recordID(0u),
		queueFamily(0u),
		// initialize allocators
		pushLables(alloc),
		accelStructCopies(alloc),
		used(alloc),
		secondaries(alloc) {
	++DebugStats::get().aliveRecords;
}

CommandRecord::~CommandRecord() {
	ZoneScoped;

	if(dev) {
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
		auto [ds, lock] = tryAccess(bds);
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
tryAccess(const BoundDescriptorSet& bds) {
	if(!bds.dsPool) {
		dlg_debug("DescriptorSet inaccessible; DescriptorSet was destroyed");
		return {};
	}

	// NOTE: even if the device mutex isn't locked, we can be sure
	// that the pool itself isn't destroyed since the associated record
	// always adds it to its used handles and thus keeps it alive.
	// assertOwned(bds.dsPool->dev->mutex);

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

// TODO: we never REALLY need the mutex here.
// But in hook.cpp we need to snapshot descriptors while having the mutex locked,
// requiring us to never lock it in the procedure, so it's designed like this.
// The proper fix would be reworking the giant commandHook critical section.
CommandDescriptorSnapshot snapshotRelevantDescriptors(Device& dev, const Command& cmd) {
	std::lock_guard lock(dev.mutex);
	return snapshotRelevantDescriptorsLocked(cmd);
}

CommandDescriptorSnapshot snapshotRelevantDescriptorsLocked(const Command& cmd) {
	CommandDescriptorSnapshot ret;

	// TODO: replace dynamic_cast with some 'isStateCmd(const Command&)'
	//  check that simply checks for category (draw | dispatch | traceRays)
	auto* scmd = dynamic_cast<const StateCmdBase*>(&cmd);
	if(!scmd) {
		return ret;
	}

	for(auto bds : scmd->boundDescriptors().descriptorSets) {
		auto [ds, lock] = tryAccess(bds);
		if(ds) {
			ret.states.emplace(bds.dsEntry, ds->addCowLocked());
		}
	}

	return ret;
}

CommandDescriptorSnapshot snapshotRelevantDescriptorsValidLocked(const Command& cmd) {
	CommandDescriptorSnapshot ret;
	auto* scmd = dynamic_cast<const StateCmdBase*>(&cmd);
	if(!scmd) {
		return ret;
	}

	for(auto bds : scmd->boundDescriptors().descriptorSets) {
		auto& ds = access(bds);
		std::lock_guard lock(ds.pool->mutex);
		ret.states.emplace(bds.dsEntry, ds.addCowLocked());
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
		dsPools(alloc),
		descriptorSets(alloc),
		images(alloc) {
}

UsedImage::UsedImage(LinAllocator& alloc) noexcept :
	RefHandle<Image>(alloc), layoutChanges(alloc) {}

} // namespace vil
