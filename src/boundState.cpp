#include <boundState.hpp>
#include <util.hpp>
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
	while(pNext) {
		auto src = static_cast<const VkBaseInStructure*>(pNext);
		auto size = structSize(src->sType);
		dlg_assertm(size > 0, "Unknown structure type!");

		auto buf = cb.allocate(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
		auto dst = reinterpret_cast<VkBaseInStructure*>(buf);
		// TODO: technicallly UB to not construct object via placement new
		// in practice, this works everywhere since its only C PODs
		std::memcpy(dst, src, size);

		if(last) {
			last->pNext = dst;
		}

		last = dst;
		pNext = src->pNext;
	}

	pNext = last;
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
	queueFamily(xcb.pool().queueFamily), images(xcb), handles(xcb), pipeLayouts(xcb) {
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
}

} // namespace fuen
