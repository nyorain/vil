#include <util/linalloc.hpp>
#include <device.hpp>

#ifdef VIL_DEBUG
	#define assertCanary(block) dlg_assert((block).canary == LinMemBlock::canaryValue);
#else
	#define assertCanary(block)
#endif // VIL_DEBUG

// TODO: We currently don't track this memory via TracyAlloc/TracyFree
// since it might be called before we can initialize tracy

namespace vil {

void freeBlocks(LinMemBlock* head) {
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

LinMemBlock& createMemBlock(size_t memSize) {
	auto totalSize = sizeof(LinMemBlock) + memSize;
	auto buf = new std::byte[totalSize]; // no need to value-initialize
	auto* memBlock = new(buf) LinMemBlock;
	memBlock->data = buf + sizeof(LinMemBlock);
	memBlock->end = memBlock->data + memSize;

	// TracyAllocS(buf, totalSize, 8);
	DebugStats::get().threadContextMem += memSize;

	return *memBlock;
}

std::byte* addBlock(LinAllocator& tc, std::size_t size, std::size_t alignment) {
	auto lastSize = memSize(*tc.memCurrent);
	auto newBlockSize = std::min<size_t>(tc.blockGrowFac * lastSize, tc.maxBlockSize);
	newBlockSize = std::max<size_t>(newBlockSize, alignPOT(size, alignment));

	auto& newBlock = createMemBlock(newBlockSize);
	newBlock.next = tc.memCurrent->next;
	tc.memCurrent->next = &newBlock;
	tc.memCurrent = &newBlock;

    std::byte* ret {};
	auto success = attemptAlloc(newBlock, size, alignment, ret);
	dlg_assert(success);
	return ret;
}

LinAllocator::LinAllocator() {
	// Already allocate the first block, other functions rely on it
	// TODO: depending on the usecase, this may be bad and very
	// unexpected. The reason we are doing this here is to keep the
	// allocation code as simple as possible, avoiding the check
	// whether there already is a block or not.
	memRoot = memCurrent = &createMemBlock(minBlockSize);
}

LinAllocator::~LinAllocator() {
	// NOTE: these must be true when the allocator was only used
	// in a scoped manner.
	// dlg_assert(memCurrent == memRoot);
	// dlg_assertm(memOffset(*memCurrent) == 0u, "{}", memOffset(*memCurrent));
	freeBlocks(memRoot);
}

} // namespace vil
