#include <threadContext.hpp>
#include <device.hpp>

namespace  vil {

std::byte* data(ThreadMemBlock& mem, size_t offset) {
	dlg_assert(offset <= mem.size);
	auto objSize = align(sizeof(mem), __STDCPP_DEFAULT_NEW_ALIGNMENT__);
	return reinterpret_cast<std::byte*>(&mem) + objSize + offset;
}

void freeBlocks(ThreadMemBlock* head) {
	if(!head) {
		return;
	}

	// Free all memory blocks
	while(head) {
		auto next = head->next;
		// no need to call MemBlocks destructor, it's trivial
		TracyFreeS(head, 8);

		if(DebugStats::instance) {
			DebugStats::instance->threadContextMem -= head->size;
		}

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

	DebugStats::instance->threadContextMem += memSize;

	return *memBlock;
}

std::byte* allocate(ThreadContext& tc, size_t size) {
	dlg_assert(bool(tc.memCurrent) == bool(tc.memRoot));
	dlg_assert(!tc.memCurrent || tc.memOffset <= tc.memCurrent->size);
	size = align(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);

	// fast path: enough memory available directly inside the command buffer,
	// simply align and advance the offset
	auto newBlockSize = size_t(tc.minBlockSize);
	dlg_assert(tc.memCurrent && tc.memRoot);
	dlg_assert(tc.memOffset % __STDCPP_DEFAULT_NEW_ALIGNMENT__ == 0u);
	if(tc.memOffset + size <= tc.memCurrent->size) {
		auto ret = data(*tc.memCurrent, tc.memOffset);
		tc.memOffset += size;
		return ret;
	}

	if(tc.memCurrent->next) {
		if(tc.memCurrent->next->size >= size) {
			tc.memCurrent = tc.memCurrent->next;
			tc.memOffset = size;
			return data(*tc.memCurrent, 0);
		}

		dlg_warn("Giant local allocation; have to free previous blocks");
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
	tc.memOffset = size;
	return data(*tc.memCurrent, 0);
}

void free(ThreadContext& tc, const std::byte* ptr, size_t size) {
	dlg_assert(tc.memCurrent);
	size = align(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);

	dlg_assert(tc.memCurrent->prev || tc.memCurrent == tc.memRoot);
	bool lastAlloc = false;
	if(tc.memOffset == 0u && tc.memCurrent->prev) {
		tc.memCurrent = tc.memCurrent->prev;
		tc.memOffset = tc.memCurrent->size;
		lastAlloc = true;
	}

	dlg_assert(tc.memOffset >= size);
	auto curr = data(*tc.memCurrent, tc.memOffset);
	dlg_assert(ptr + size <= curr);
	dlg_assert(lastAlloc || ptr + size == curr);

	tc.memOffset -= size;
}

ThreadContext& ThreadContext::get() {
	static thread_local ThreadContext tc;
	return tc;
}

ThreadContext::ThreadContext() {
	// already allocate the first block, others may rely on it
	// See e.g. ThreadMemScope.
	memRoot = memCurrent = &createMemBlock(minBlockSize, nullptr);
}

ThreadContext::~ThreadContext() {
	dlg_assertm(memOffset == 0u, "{}", memOffset);
	dlg_assert(memCurrent == memRoot);
	freeBlocks(memRoot);
}

ThreadMemScope::ThreadMemScope() {
	auto& tc = ThreadContext::get();
	block = tc.memCurrent;
	offset = tc.memOffset;
}

ThreadMemScope::~ThreadMemScope() {
	auto& tc = ThreadContext::get();

	// make sure that all memory in between did come from this
	dlg_check({
		auto off = offset;
		auto size = sizeAllocated;
		auto it = block;
		while(off + size > block->size) {
			dlg_assert(it->next);
			dlg_assert(off <= block->size);
			size -= block->size - off;
			off = 0u;
			it = it->next;
		}

		dlg_assert(it == tc.memCurrent);
		dlg_assertm(tc.memOffset == off + size,
			"{} != {} (off {}, size {})", tc.memOffset, off + size, off, size);
	});

	tc.memCurrent = block;
	tc.memOffset = offset;
}

} // namespace vil
