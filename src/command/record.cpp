#include <command/record.hpp>
#include <command/commands.hpp>
#include <command/alloc.hpp>
#include <commandHook/record.hpp>
#include <commandHook/hook.hpp>
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
#include <gencmd.hpp>
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

void clearHookRecordsLocked(CommandRecord& rec) {
	dlg_assert(rec.dev);
	assertOwned(rec.dev->mutex);

	ZoneScopedN("clear hookRecords");
	auto moved = std::move(rec.hookRecords); // move to allow iteration
	for(auto* record : moved) {
		dlg_assert(!record->invalid);
		dlg_assert(!record->writer); // wouldn't make sense
		rec.dev->commandHook->removeRecordLocked(*record);
	}

	rec.hookRecords = {};
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
		clearHookRecordsLocked(*this);
	} else {
		dlg_assert(hookRecords.empty());
	}

	dlg_assert(DebugStats::get().aliveRecords > 0);
	--DebugStats::get().aliveRecords;
}

// util
void bindPushDescriptors(Device& dev, VkCommandBuffer cb, VkPipelineBindPoint bbp,
		BoundDescriptorSet& bds, u32 setID) {
	auto& dsLayout = bds.layout->descriptors[setID];
	dlg_assert(dsLayout->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT);

	auto dsData = DescriptorStateRef{};
	dsData.layout = dsLayout.get();
	dsData.data = bds.pushDescriptors.data();

	ThreadMemScope tms;
	auto writes = tms.alloc<VkWriteDescriptorSet>(dsLayout->bindings.size());
	for(auto [i, bindingLayout] : enumerate(dsLayout->bindings)) {
		auto& dst = writes[i];

		dst = {};
		dst.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		dst.descriptorType = bindingLayout.descriptorType;
		dst.dstBinding = i;
		dst.descriptorCount = bindingLayout.descriptorCount;

		switch(category(dst.descriptorType)) {
			case DescriptorCategory::buffer: {
				auto dstBufs = tms.alloc<VkDescriptorBufferInfo>(dst.descriptorCount);
				auto srcBufs = buffers(dsData, i);
				for(auto i = 0u; i < dst.descriptorCount; ++i) {
					auto* buf = srcBufs[i].buffer; dstBufs[i].buffer = buf ? buf->handle : VK_NULL_HANDLE;
					dstBufs[i].offset = srcBufs[i].offset;
					dstBufs[i].range = srcBufs[i].range;
				}
				dst.pBufferInfo = dstBufs.data();
				break;
			} case DescriptorCategory::image: {
				auto dstImages = tms.alloc<VkDescriptorImageInfo>(dst.descriptorCount);
				auto srcImages = images(dsData, i);
				for(auto i = 0u; i < dst.descriptorCount; ++i) {
					auto* iv = srcImages[i].imageView;
					auto* sampler = srcImages[i].sampler;
					dstImages[i].imageView = iv ? iv->handle : VK_NULL_HANDLE;
					dstImages[i].sampler = sampler ? sampler->handle : VK_NULL_HANDLE;
					dstImages[i].imageLayout = srcImages[i].layout;
				}
				dst.pImageInfo = dstImages.data();
				break;
			} case DescriptorCategory::bufferView: {
				auto dstViews = tms.alloc<VkBufferView>(dst.descriptorCount);
				auto srcViews = bufferViews(dsData, i);
				for(auto i = 0u; i < dst.descriptorCount; ++i) {
					auto* bv = srcViews[i].bufferView;
					dstViews[i] = bv ? bv->handle : VK_NULL_HANDLE;
				}
				dst.pTexelBufferView = dstViews.data();
				break;
			} case DescriptorCategory::accelStruct: {
				auto& asWrite = tms.construct<VkWriteDescriptorSetAccelerationStructureKHR>();
				asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;

				auto dstAS = tms.alloc<VkAccelerationStructureKHR>(dst.descriptorCount);
				auto srcAS = accelStructs(dsData, i);
				for(auto i = 0u; i < dst.descriptorCount; ++i) {
					auto* as = srcAS[i].accelStruct;
					dstAS[i] = as ? as->handle : VK_NULL_HANDLE;
				}

				asWrite.pAccelerationStructures = dstAS.data();
				asWrite.accelerationStructureCount = dstAS.size();
				dst.pNext = &asWrite;
				break;
			} default:
				dlg_error("Invalid/unknown descriptor type");
				break;
		}
	}

	dev.dispatch.CmdPushDescriptorSet(cb, bbp, bds.layout->handle,
		setID, writes.size(), writes.data());
}

void bind(Device& dev, VkCommandBuffer cb, const ComputeState& state) {
	assertOwned(dev.mutex);

	if(state.pipe) {
		dev.dispatch.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
			state.pipe->handle);
	}

	for(auto i = 0u; i < state.descriptorSets.size(); ++i) {
		auto& bds = state.descriptorSets[i];
		if(!bds.dsPool && bds.layout) {
			dlg_assert(!bds.dsEntry);

			// NOTE: we only need this since we don't track this during recording
			// anymore at the moment.
			if(state.pipe && !compatibleForSetN(*state.pipe->layout,
					*bds.layout, i)) {
				dlg_info("incompatible set {}", i);
				break;
			}

			bindPushDescriptors(dev, cb, VK_PIPELINE_BIND_POINT_COMPUTE,
				bds, i);
			continue;
		}

		auto [ds, lock] = tryAccess(bds);
		dlg_assert(ds);

		// NOTE: we only need this since we don't track this during recording
		// anymore at the moment.
		if(state.pipe && !compatibleForSetN(*state.pipe->layout,
				*bds.layout, i)) {
			dlg_info("incompatible set {}", i);
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
	if(!bds.layout) {
		dlg_debug("DescriptorSet never bound");
		return {};
	}

	if(!bds.dsPool) {
		dlg_warn("Push Descriptor?");
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

[[nodiscard]]
std::pair<DescriptorStateRef, std::unique_lock<LockableBase(DebugMutex)>>
tryAccessState(const DescriptorState& state, u32 setID) {
	dlg_assert_or(setID < state.descriptorSets.size(), return {});

	auto& bds = state.descriptorSets[setID];
	if (!bds.layout) { // was never bound
		return {};
	}

	auto& setLayout = bds.layout->descriptors[setID];

	dlg_assert_or(!!bds.dsPool == !!bds.dsEntry, return {});
	if (!bds.dsEntry) {
		dlg_assert(setLayout->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT);
		DescriptorStateRef ref;
		ref.data = bds.pushDescriptors.data();
		ref.layout = setLayout.get();
		ref.variableDescriptorCount = 0u;
		return {ref, std::unique_lock<LockableBase(DebugMutex)>{}};
	}

	auto [ds, lock] = tryAccess(bds);
	if (!ds) {
		return {};
	}

	return {DescriptorStateRef(*ds), std::move(lock)};
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
		if (!bds.dsEntry) {
			continue; // push descriptor or not bound
		}

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
		shaderObjects(alloc),
		indirectCommandLayouts(alloc),
		indirectExecutionSets(alloc),
		descriptorSets(alloc),
		images(alloc) {
}

UsedImage::UsedImage(LinAllocator& alloc) noexcept :
	RefHandle<Image>(alloc), layoutChanges(alloc) {}

span<const VkViewport> viewports(const GraphicsState& state) {
	if(!state.pipe) {
		return state.dynamic.viewports;
	}

	if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_VIEWPORT)) {
		return state.dynamic.viewports;
	}

	return state.pipe->viewports;
}

std::pair<DescriptorStateRef, std::unique_lock<DebugMutex>> accessSet(
		const DescriptorState& state, u32 set, const CommandDescriptorSnapshot& snapshot) {
	if (state.descriptorSets.size() < set) {
		return {};
	}

	auto& boundSet = state.descriptorSets[set];
	if (!boundSet.layout) {
		return {};
	}

	dlg_assert(set < boundSet.layout->descriptors.size());
	auto& setLayout = boundSet.layout->descriptors[set];

	if (!boundSet.dsEntry) {
		dlg_assert(setLayout->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT);
		DescriptorStateRef ref;
		ref.data = boundSet.pushDescriptors.data();
		ref.layout = setLayout.get();
		ref.variableDescriptorCount = 0u;
		return {ref, std::unique_lock<DebugMutex>{}};
	}

	auto stateIt = snapshot.states.find(boundSet.dsEntry);
	if(stateIt == snapshot.states.end()) {
		// not 100% sure how this can happen
		dlg_error("uncaptured set");
		return {};
	}

	auto& dsCow = *stateIt->second;
	auto ret = access(dsCow);

	dlg_assert(compatible(*setLayout, *ret.first.layout));
	return ret;
}

} // namespace vil
