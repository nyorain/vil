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
		span<DescriptorSet* const> sets, span<const u32>) {
	ensureSize(cb, descriptorSets, firstSet + sets.size());

	// NOTE: the "ds disturbing" part of vulkan is hard to grasp IMO.
	// There may be errors here.
	// TODO: do we even need to track it like this? only useful if we
	// also show it in UI which sets were disturbed.

	auto lastSet = firstSet + sets.size() - 1;
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

	// bind descriptors and check if future bindings are disturbed
	auto followingDisturbed = false;
	for(auto i = 0u; i < sets.size(); ++i) {
		auto s = firstSet + i;
		if(!descriptorSets[s].ds || !compatibleForSetN(*descriptorSets[s].layout, layout, s)) {
			followingDisturbed = true;
		}

		descriptorSets[s].layout = &layout;
		descriptorSets[s].ds = sets[i];
		// TODO: use given offsets. We have to analyze the layout and
		// count the offset into the offsets array.
		descriptorSets[s].dynamicOffsets = {};
	}

	if(followingDisturbed) {
		// dlg_debug("disturbed following descriptorSets, from {}", lastSet + 1);
		descriptorSets = descriptorSets.subspan(0, lastSet + 1);
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
		cbName = copyString(*cb, cb->name);
	}
}

CommandRecord::~CommandRecord() {
	if(!dev) {
		return;
	}

	std::lock_guard lock(dev->mutex);

	if(group) {
		auto count = group->aliveRecords.erase(this);
		dlg_assertm(count, "CommandRecord not found in its group");
	}

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
	hook.reset();
}

void replaceInvalidatedLocked(CommandRecord& record) {
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

void notifyInvalidateLocked(CommandRecord& record, const DescriptorSet& ds) {
	if(record.keepDescriptorInformation) {
		// TODO: many memory copies, memory allocations, too expensive here
		auto& newDS = record.keptDescriptors.emplace_back();
		newDS.layout = ds.layout;
		newDS.bindings = ds.bindings;
		newDS.name = ds.name; // TODO: needed?
	}
}

} // namespace vil
