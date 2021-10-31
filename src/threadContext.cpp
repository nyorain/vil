#include <threadContext.hpp>
#include <device.hpp>

#ifdef VIL_DEBUG
	#define assertCanary(block) dlg_assert((block).canary == ThreadMemBlock::canaryValue);
#else
	#define assertCanary(block)
#endif // VIL_DEBUG

namespace vil {

void freeBlocks(ThreadMemBlock* head) {
	if(!head) {
		return;
	}

	// Free all memory blocks
	while(head) {
		assertCanary(*head);
		auto next = head->next;
		// no need to call MemBlocks destructor, it's trivial
		TracyFreeS(head, 8);

		DebugStats::get().threadContextMem -= head->size;

		delete[] reinterpret_cast<std::byte*>(head);
		head = next;
	}
}

ThreadMemBlock& createMemBlock(size_t memSize, ThreadMemBlock* prev) {
	auto totalSize = align(sizeof(ThreadMemBlock), __STDCPP_DEFAULT_NEW_ALIGNMENT__) + memSize;
	auto buf = new std::byte[totalSize];
	TracyAllocS(buf, totalSize, 8);
	auto* memBlock = new(buf) ThreadMemBlock;
	memBlock->size = memSize;
	memBlock->prev = prev;
	memBlock->next = nullptr;

	DebugStats::get().threadContextMem += memSize;

	return *memBlock;
}

// Guaranteed to be aligned with __STDCPP_DEFAULT_NEW_ALIGNMENT__
/*
std::byte* allocate(ThreadContext& tc, size_t size) {
	dlg_assert(tc.memCurrent); // there is always one
	dlg_assert(tc.memCurrent->offset <= tc.memCurrent->size);
	size = align(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);

	assertCanary(*tc.memCurrent);

	// fast path: enough memory available directly inside the command buffer,
	// simply align and advance the offset
	auto newBlockSize = size_t(tc.minBlockSize);
	dlg_assert(tc.memCurrent && tc.memRoot);
	dlg_assert(tc.memCurrent->offset % __STDCPP_DEFAULT_NEW_ALIGNMENT__ == 0u);
	if(tc.memCurrent->offset + size <= tc.memCurrent->size) {
		auto ret = data(*tc.memCurrent, tc.memCurrent->offset);
		tc.memCurrent->offset += size;
		return ret;
	}

	if(tc.memCurrent->next) {
		if(tc.memCurrent->next->size >= size) {
			tc.memCurrent = tc.memCurrent->next;
			tc.memCurrent->offset = size;
			return data(*tc.memCurrent, 0);
		}

		// TODO: Just insert it in between, no need to free blocks here I guess
		dlg_warn("Giant local allocation (size {}); have to free previous blocks", size);
		freeBlocks(tc.memCurrent->next);
		tc.memCurrent->next = nullptr;
	}

	// not enough memory available in last block, allocate new one
	newBlockSize = std::min<size_t>(tc.blockGrowFac * tc.memCurrent->size, tc.maxBlockSize);
	newBlockSize = std::max<size_t>(newBlockSize, size);

	dlg_assert(!tc.memCurrent->next);
	auto& newBlock = createMemBlock(newBlockSize, tc.memCurrent);
	tc.memCurrent->next = &newBlock;

	tc.memCurrent = &newBlock;
	tc.memCurrent->offset = size;
	return data(*tc.memCurrent, 0);
}
*/

// NOTE: legacy interface, might be useful in future.
/*
void free(ThreadContext& tc, const std::byte* ptr, size_t size) {
	dlg_assert(tc.memCurrent);
	size = align(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);

	dlg_assert(tc.memCurrent->prev || tc.memCurrent == tc.memRoot);
	while(tc.memCurrent->offset == 0u && tc.memCurrent->prev) {
		tc.memCurrent = tc.memCurrent->prev;
	}

	dlg_assert(tc.memCurrent->offset >= size);
	auto curr = data(*tc.memCurrent, tc.memCurrent->offset);
	dlg_assert(ptr + size == curr);

	tc.memCurrent->offset -= size;
}
*/

// ThreadContext& ThreadContext::get() {
// 	static thread_local ThreadContext tc;
// 	return tc;
// }

ThreadContext::ThreadContext() {
	// already allocate the first block, others may rely on it
	// See e.g. ThreadMemScope.
	memRoot = memCurrent = &createMemBlock(minBlockSize, nullptr);
}

ThreadContext::~ThreadContext() {
	dlg_assert(memCurrent == memRoot);
	dlg_assertm(memCurrent->offset == 0u, "{}", memCurrent->offset);
	freeBlocks(memRoot);
}

/*
ThreadMemScope::ThreadMemScope() {
	auto& tc = ThreadContext::get();
	block = tc.memCurrent;
	offset = block->offset;

#ifdef VIL_DEBUG
	current = data(*block, offset);
#endif // VIL_DEBUG
}
*/

/*
ThreadMemScope::~ThreadMemScope() {
	auto& tc = ThreadContext::get();

	// make sure that all memory in between did come from us
#ifdef VIL_DEBUG
	// starting at the allocation point we stored when 'this' was
	// constructed...
	auto off = this->offset;
	auto size = this->sizeAllocated;
	auto it = this->block;

	// ...iterate through the blocks allocated since then...
	while(off + size > it->offset) {
		dlg_assert(it->canary == ThreadMemBlock::canaryValue);
		dlg_assertm(it->next, "remaining: {}", size);
		dlg_assertm(off <= it->offset, "{} vs {}", off, it->offset);
		size -= it->offset - off;
		off = 0u;
		it = it->next;
	}

	// ...and assert that we would land at the current allocation point
	dlg_assert(it->canary == ThreadMemBlock::canaryValue);
	dlg_assert(it == tc.memCurrent);
	dlg_assertm(it->offset == off + size,
		"{} != {} (off {}, size {})", it->offset, off + size, off, size);
#endif // VIL_DEBUG

	tc.memCurrent = block;
	tc.memCurrent->offset = offset;
}
*/

/*
std::byte* ThreadMemScope::allocRaw(size_t size) {
	auto& tc = ThreadContext::get();

#ifdef VIL_DEBUG
	dlg_assertm(data(*tc.memCurrent, tc.memCurrent->offset) == this->current,
		"Invalid non-stacking interleaving of ThreadMemScope detected");
#endif // VIL_DEBUG

	auto* ptr = vil::allocate(tc, size);

#ifdef VIL_DEBUG
	current = ptr + align(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
	sizeAllocated += align(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
#endif // VIL_DEBUG

	return ptr;
}
*/

} // namespace vil
