#include <command/record.hpp>
#include <command/commands.hpp>
#include <image.hpp>
#include <pipe.hpp>
#include <cb.hpp>
#include <ds.hpp>
#include <util/util.hpp>
#include <gui/commandHook.hpp>

namespace vil {

void DescriptorState::bind(CommandBuffer& cb, PipelineLayout& layout, u32 firstSet,
		span<DescriptorSet* const> sets, span<const u32> dynOffsets) {
	ensureSize0(cb, descriptorSets, firstSet + sets.size());

	// NOTE: the "ds disturbing" part of vulkan is hard to grasp IMO.
	// There may be errors here.
	// TODO PERF: do we even need to track it like this? only useful if we
	// also show it in UI which sets were disturbed.
	// Disabled for now

// #define DS_DISTURB_CHECKS
#ifdef DS_DISTURB_CHECKS
	for(auto i = 0u; i < firstSet; ++i) {
		if(!descriptorSets[i].ds) {
			continue;
		}

		dlg_assert(descriptorSets[i].layout);
		if(!compatibleForSetN(*descriptorSets[i].layout, layout, i)) {
			// disturbed!
			// dlg_debug("disturbed ds {}", i);
			descriptorSets[i] = {};
		}
	}
#endif // DS_DISTURB_CHECKS

	// bind descriptors and check if future bindings are disturbed
	auto followingDisturbed = false;
	for(auto i = 0u; i < sets.size(); ++i) {
		auto s = firstSet + i;
		auto& dsLayout = *layout.descriptors[s];

#ifdef DS_DISTURB_CHECKS
		if(!descriptorSets[s].layout || !compatibleForSetN(*descriptorSets[s].layout, layout, s)) {
			followingDisturbed = true;
		}
#endif // DS_DISTURB_CHECKS

		descriptorSets[s].layout = &layout;
		descriptorSets[s].ds = sets[i];

		dlg_assert(dsLayout.numDynamicBuffers <= dynOffsets.size());
		descriptorSets[s].dynamicOffsets = copySpan(cb, dynOffsets.data(), dsLayout.numDynamicBuffers);
		dynOffsets.subspan(dsLayout.numDynamicBuffers);
	}

	if(followingDisturbed) {
		// dlg_debug("disturbed following descriptorSets, from {}", lastSet + 1);
		for(auto i = firstSet + sets.size(); i < descriptorSets.size(); ++i) {
			descriptorSets[i] = {};
		}
	}
}

// NOTE: We need them to be noexcept. We can't rely on the internal
//   std::list to be noexcept movable though. So we implement it ourselves,
//   effectively intentionally crashing the program (throwing from noexcept)
//   when an stl throws from the move operation.
//   Could be fixed later on with our own linked-list/grid.
UsedImage::UsedImage(UsedImage&& rhs) noexcept :
		image(rhs.image),
		layoutChanged(rhs.layoutChanged),
		finalLayout(rhs.finalLayout),
		commands(std::move(rhs.commands)) {
}

UsedImage& UsedImage::operator=(UsedImage&& rhs) noexcept {
	image = rhs.image;
	layoutChanged = rhs.layoutChanged;
	finalLayout = rhs.finalLayout;
	commands = std::move(rhs.commands);
	return *this;
}

UsedHandle::UsedHandle(UsedHandle&& rhs) noexcept :
		handle(rhs.handle),
		commands(std::move(rhs.commands)) {
}

UsedHandle& UsedHandle::operator=(UsedHandle&& rhs) noexcept {
	handle = rhs.handle;
	commands = std::move(rhs.commands);
	return *this;
}

// Record
CommandRecord::CommandRecord(CommandBuffer& xcb) :
		dev(xcb.dev),
		cb(&xcb),
		recordID(xcb.recordCount()),
		queueFamily(xcb.pool().queueFamily),
		// initialize allocators
		pushLables(*this),
		images(*this),
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

		// remove cb from all referenced resources
		auto removeFromResource = [&](auto& res) {
			if(invalidated.count(&res) == 0) {
				[[maybe_unused]] auto count = res.refRecords.erase(this);
				dlg_assert(count > 0);
			}
		};

		for(auto& img : images) {
			removeFromResource(*img.second.image);
		}

		for(auto& handle : handles) {
			removeFromResource(*handle.second.handle);
		}

		// Its destructor might reference this.
		// And it must be called while mutex is locked.
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
	// NOTE: there is no need to insert the new handles (the old ones
	// are replaced to) since that mechanism is only used for dummy descriptor
	// sets and we don't need to explicitly track them in usedHandles.
	for(auto it = record.handles.begin(); it != record.handles.end(); ) {
		if(record.invalidated.count(it->second.handle)) {
			it = record.handles.erase(it);
		} else {
			++it;
		}
	}

	for(auto it = record.images.begin(); it != record.images.end(); ) {
		if(record.invalidated.count(it->second.image)) {
			it = record.images.erase(it);
		} else {
			++it;
		}
	}

	record.invalidated.clear();
}

// util
void copy(CommandBuffer& cb, const DescriptorState& src, DescriptorState& dst) {
	dst.descriptorSets = copySpan(cb, src.descriptorSets);
	dst.pushDescriptors = copySpan(cb, src.pushDescriptors);
}

GraphicsState copy(CommandBuffer& cb, const GraphicsState& src) {
	GraphicsState dst = src;
	copy(cb, src, dst); // descriptors

	dst.vertices = copySpan(cb, src.vertices);
	dst.dynamic.viewports = copySpan(cb, src.dynamic.viewports);
	dst.dynamic.scissors = copySpan(cb, src.dynamic.scissors);

	return dst;
}

ComputeState copy(CommandBuffer& cb, const ComputeState& src) {
	ComputeState dst = src;
	copy(cb, src, dst); // descriptors
	return dst;
}

RayTracingState copy(CommandBuffer& cb, const RayTracingState& src) {
	RayTracingState dst = src;
	copy(cb, src, dst); // descriptors
	return dst;
}

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

} // namespace vil
