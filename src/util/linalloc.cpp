#include <util/linalloc.hpp>
#include <util/util.hpp> // nextPOT
#include <device.hpp>

#ifdef VIL_DEBUG
	#define assertCanary(block) dlg_assert((block).canary == LinMemBlock::canaryValue);
#else
	#define assertCanary(block)
#endif // VIL_DEBUG

namespace vil {

std::byte* LinAllocator::addBlock(std::size_t size, std::size_t alignment) {
	auto newBlockSize = (memCurrent == &memRoot) ? minBlockSize :
		std::min<size_t>(blockGrowFac * memSize(*memCurrent), maxBlockSize);
	auto neededSize = alignPOT(size, alignment) + sizeof(LinMemBlock);
	newBlockSize = nextPOT(std::max<size_t>(newBlockSize, neededSize));

	auto buf = new std::byte[newBlockSize]; // no need to value-initialize
	auto* newBlock = new(buf) LinMemBlock;
	newBlock->data = buf + sizeof(LinMemBlock);
	newBlock->end = buf + newBlockSize;

	if(onAlloc) {
		onAlloc(buf, newBlockSize);
	}

	newBlock->next = memCurrent->next;
	memCurrent->next = newBlock;
	memCurrent = newBlock;

    std::byte* ret {};
	auto success = attemptAlloc(*newBlock, size, alignment, ret);
	dlg_assert(success);
	return ret;
}

LinAllocator::LinAllocator() {
	// We intentionally start with the empty block as current block.
	// This way we don't have to allocate memory on construction (which is undesireable)
	// and don't have to do a special null-block handling in allocate
	memCurrent = &memRoot;
}

LinAllocator::LinAllocator(Callback alloc, Callback free) : LinAllocator() {
	onAlloc = alloc;
	onFree = free;
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

		auto ptr = reinterpret_cast<std::byte*>(head);
		if(onFree) {
			onFree(ptr, sizeof(LinMemBlock) + memSize(*head));
		}

		// no need to call MemBlocks destructor, it's trivial
		static_assert(std::is_trivially_destructible_v<LinMemBlock>);
		delete[] ptr;
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
