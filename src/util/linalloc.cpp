#include <util/linalloc.hpp>
#include <device.hpp>

#ifdef VIL_DEBUG
	#define assertCanary(block) dlg_assert((block).canary == LinMemBlock::canaryValue);
#else
	#define assertCanary(block)
#endif // VIL_DEBUG

namespace vil {

LinMemBlock& createMemBlock(size_t memSize) {
	auto totalSize = sizeof(LinMemBlock) + memSize;
	auto buf = new std::byte[totalSize]; // no need to value-initialize
	auto* memBlock = new(buf) LinMemBlock;
	memBlock->data = buf + sizeof(LinMemBlock);
	memBlock->end = memBlock->data + memSize;

	TracyAllocS(buf, totalSize, 8);
	DebugStats::get().threadContextMem += memSize;

	return *memBlock;
}

std::byte* LinAllocator::addBlock(std::size_t size, std::size_t alignment) {
	auto lastSize = memSize(*memCurrent);
	auto newBlockSize = std::min<size_t>(blockGrowFac * lastSize, maxBlockSize);
	newBlockSize = std::max<size_t>(newBlockSize, alignPOT(size, alignment));

	auto& newBlock = createMemBlock(newBlockSize);
	newBlock.next = memCurrent->next;
	memCurrent->next = &newBlock;
	memCurrent = &newBlock;

    std::byte* ret {};
	auto success = attemptAlloc(newBlock, size, alignment, ret);
	dlg_assert(success);
	return ret;
}

LinAllocator::LinAllocator() {
	memCurrent = &memRoot;
}

LinAllocator::~LinAllocator() {
	release();
}

void LinAllocator::reset() {
	// Reset all memory blocks
	auto head = memRoot.next;
	while(head) {
		head->data = dataBegin(*head);
		head = head->next;
	}

	// NOTE: could reset `memCurrent = memRoot.next` if it exists but
	// we don't do so, e.g. to keep the empty() implementation simple.
	memCurrent = &memRoot;
}

void LinAllocator::release() {
	// Free all memory blocks
	auto head = memRoot.next;
	while(head) {
		assertCanary(*head);
		auto next = head->next;
		// no need to call MemBlocks destructor, it's trivial

		TracyFreeS(head, 8);
		DebugStats::get().threadContextMem -= memSize(*head);

		delete[] reinterpret_cast<std::byte*>(head);
		head = next;
	}

	memRoot.next = nullptr;
	memCurrent = &memRoot;
}

bool LinAllocator::empty() const {
	// dlg_assertm(memOffset(*memCurrent) == 0u, "{}", memOffset(*memCurrent));
	return (memCurrent == &memRoot);
}

} // namespace vil
