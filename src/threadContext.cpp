#include <threadContext.hpp>

namespace  vil {

std::byte* data(ThreadMemBlock& mem, size_t offset) {
	dlg_assert(offset <= mem.size);
	return reinterpret_cast<std::byte*>(&mem) + sizeof(mem) + offset;
}

void freeBlocks(ThreadMemBlock* head) {
	if(!head) {
		return;
	}

	// Free all memory blocks
	while(head) {
		auto next = head->next;
		// no need to call MemBlocks destructor, it's trivial
		delete[] reinterpret_cast<std::byte*>(head);
		head = next;
	}
}

ThreadMemBlock& createMemBlock(size_t memSize, ThreadMemBlock* prev) {
	auto buf = new std::byte[sizeof(ThreadMemBlock) + memSize];
	auto* memBlock = new(buf) ThreadMemBlock;
	memBlock->size = memSize;
	memBlock->prev = prev;
	memBlock->next = nullptr;
	return *memBlock;
}

std::byte* allocate(ThreadContext& tc, size_t size) {
	dlg_assert(bool(tc.memCurrent) == bool(tc.memRoot));
	dlg_assert(!tc.memCurrent || tc.memOffset <= tc.memCurrent->size);
	size = align(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);

	// We grow block sizes exponentially, up to a maximum
	constexpr auto minBlockSize = 16 * 1024;
	constexpr auto maxBlockSize = 16 * 1024 * 1024;
	constexpr auto blockGrowFac = 2;

	// fast path: enough memory available directly inside the command buffer,
	// simply align and advance the offset
	auto newBlockSize = size_t(minBlockSize);
	if(tc.memCurrent) {
		dlg_assert(tc.memOffset == align(tc.memOffset, __STDCPP_DEFAULT_NEW_ALIGNMENT__));
		auto remaining = i64(tc.memCurrent->size) - i64(tc.memOffset);
		if(remaining >= i64(size)) {
			auto ret = data(*tc.memCurrent, tc.memOffset);
			tc.memOffset += size;
			return ret;
		}

		newBlockSize = std::min<size_t>(blockGrowFac * tc.memCurrent->size, maxBlockSize);
	}

	// not enough memory available in last block, allocate new one
	newBlockSize = std::max<size_t>(newBlockSize, size);
	auto newBlock = createMemBlock(newBlockSize, tc.memCurrent);
	if(tc.memCurrent) {
		tc.memCurrent->next = &newBlock;
	} else {
		dlg_assert(!tc.memRoot);
		tc.memRoot = &newBlock;
	}

	tc.memOffset = size;
	tc.memCurrent = &newBlock;
	return data(*tc.memCurrent, 0);
}

void free(ThreadContext& tc, const std::byte* ptr, size_t size) {
	dlg_assert(tc.memCurrent);
	size = align(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);

	dlg_assert(tc.memOffset >= size);
	auto curr = data(*tc.memCurrent, tc.memOffset);
	dlg_assert(ptr + size == curr);
	tc.memOffset -= size;

	if(tc.memOffset == 0u && tc.memCurrent->prev) {
		tc.memCurrent = tc.memCurrent->prev;
	}
}

ThreadContext& ThreadContext::get() {
	static thread_local ThreadContext tc;
	return tc;
}

} // namespace vil
