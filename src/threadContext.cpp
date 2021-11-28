#include <threadContext.hpp>
#include <device.hpp>

#ifdef VIL_DEBUG
	#define assertCanary(block) dlg_assert((block).canary == ThreadMemBlock::canaryValue);
#else
	#define assertCanary(block)
#endif // VIL_DEBUG

// TODO: We currently don't track this memory via TracyAlloc/TracyFree
// since it might be called before we can initialize tracy

namespace vil {

thread_local ThreadContext ThreadContext::instance;

void freeBlocks(ThreadMemBlock* head) {
	if(!head) {
		return;
	}

	// Free all memory blocks
	while(head) {
		assertCanary(*head);
		auto next = head->next;
		// no need to call MemBlocks destructor, it's trivial
		// TracyFreeS(head, 8);

		DebugStats::get().threadContextMem -= memSize(*head);

		delete[] reinterpret_cast<std::byte*>(head);
		head = next;
	}
}

ThreadMemBlock& createMemBlock(size_t memSize) {
	auto totalSize = sizeof(ThreadMemBlock) + memSize;
	auto buf = new std::byte[totalSize]; // no need to value-initialize
	auto* memBlock = new(buf) ThreadMemBlock;
	memBlock->data = buf + sizeof(ThreadMemBlock);
	memBlock->end = memBlock->data + memSize;

	// TracyAllocS(buf, totalSize, 8);
	DebugStats::get().threadContextMem += memSize;

	return *memBlock;
}

std::byte* addBlock(ThreadContext& tc, std::size_t size, std::size_t alignment) {
	auto lastSize = memSize(*tc.memCurrent);
	auto newBlockSize = std::min<size_t>(tc.blockGrowFac * lastSize, tc.maxBlockSize);
	newBlockSize = std::max<size_t>(newBlockSize, alignPOT(size, alignment));

	auto& newBlock = createMemBlock(newBlockSize);
	newBlock.prev = tc.memCurrent;
	newBlock.next = tc.memCurrent->next;

	if(tc.memCurrent->next) {
		tc.memCurrent->next->prev = &newBlock;
	}

	tc.memCurrent->next = &newBlock;
	tc.memCurrent = &newBlock;

	auto ret = attemptAlloc(newBlock, size, alignment);
	dlg_assert(ret);
	return ret;
}

ThreadContext::ThreadContext() {
	// already allocate the first block, others may rely on it
	// See e.g. ThreadMemScope.
	memRoot = memCurrent = &createMemBlock(minBlockSize);
}

ThreadContext::~ThreadContext() {
	dlg_assert(memCurrent == memRoot);
	dlg_assertm(memOffset(*memCurrent) == 0u, "{}", memOffset(*memCurrent));
	freeBlocks(memRoot);
}

} // namespace vil
