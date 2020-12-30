#include <record.hpp>
#include <util.hpp>
#include <commands.hpp>
#include <image.hpp>
#include <cb.hpp>

namespace fuen {

void returnBlocks(CommandPool& pool, CommandMemBlock* head) {
	if(!head) {
		return;
	}

	auto tail = head;
	auto next = tail;
	while((next = tail->next.load())) {
		tail = next;
	}

	// auto oldHead = pool.memBlocks.load();
	// tail->next.store(oldHead);

	auto oldHead = static_cast<CommandMemBlock*>(nullptr);
	while(!pool.memBlocks.compare_exchange_weak(oldHead, head)) {
		tail->next.store(oldHead);
	}
}

void freeBlocks(CommandMemBlock* head) {
	if(!head) {
		return;
	}

	// Free all memory blocks
	while(head) {
		auto next = head->next.load();
		// no need to call MemBlocks destructor, it's trivial
		delete[] reinterpret_cast<std::byte*>(head);
		head = next;
	}
}

void copyChain(CommandBuffer& cb, const void*& pNext) {
	VkBaseInStructure* last = nullptr;
	auto it = pNext;
	while(it) {
		auto src = static_cast<const VkBaseInStructure*>(pNext);
		auto size = structSize(src->sType);
		dlg_assertm(size > 0, "Unknown structure type!");

		auto buf = cb.allocate(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
		auto dst = reinterpret_cast<VkBaseInStructure*>(buf);
		// TODO: technicallly UB to not construct object via placement new.
		// In practice, this works everywhere since its only C PODs
		std::memcpy(dst, src, size);

		if(last) {
			last->pNext = dst;
		} else {
			pNext = dst;
		}

		last = dst;
		pNext = src->pNext;
	}
}

// Record
void MemBlockDeleter::operator()(CommandMemBlock* blocks) {
	if(!dev) {
		dlg_assert(!blocks);
		return;
	}

	if(blocks) {
		std::lock_guard lock(dev->mutex);
		if(pool) {
			[[maybe_unused]] auto count = pool->records.erase(this);
			dlg_assert(count > 0);
			returnBlocks(*pool, blocks);
		} else {
			freeBlocks(blocks);
		}
	}
}

CommandRecord::CommandRecord(CommandBuffer& xcb) :
	memBlocks(nullptr, {xcb.dev, &xcb.pool()}), cb(&xcb), recordID(xcb.recordCount()),
	queueFamily(xcb.pool().queueFamily), images(xcb), handles(xcb),
	pipeLayouts(xcb), secondaries(xcb) {
}

CommandRecord::~CommandRecord() {
	auto dev = memBlocks.get_deleter().dev;
	if(!dev) {
		return;
	}

	std::lock_guard lock(dev->mutex);

	// remove cb from all referenced resources
	auto removeFromResource = [&](auto& res) {
		if(destroyed.count(&res) == 0) {
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

	// just to be safe, its destructor might reference this
	hook.reset();
}

void unsetDestroyedLocked(CommandRecord& record) {
	if(record.destroyed.empty()) {
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
		cmd->unset(record.destroyed);
		cmd = cmd->next;
	}

	// remove from handles
	for(auto it = record.handles.begin(); it != record.handles.end(); ) {
		if(record.destroyed.count(it->second.handle)) {
			it = record.handles.erase(it);
		} else {
			++it;
		}
	}

	for(auto it = record.images.begin(); it != record.images.end(); ) {
		if(record.destroyed.count(it->second.image)) {
			it = record.images.erase(it);
		} else {
			++it;
		}
	}

	record.destroyed.clear();
}

} // namespace fuen
