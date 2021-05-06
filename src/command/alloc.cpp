#include <command/alloc.hpp>
#include <util/util.hpp>
#include <cb.hpp>

namespace vil {

std::byte* data(CommandMemBlock& mem, size_t offset) {
	dlg_assert(offset < mem.size);
	return reinterpret_cast<std::byte*>(&mem) + sizeof(mem) + offset;
}

void freeBlocks(CommandMemBlock* head) {
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

CommandMemBlock& createMemBlock(size_t memSize, CommandMemBlock* next) {
	auto buf = new std::byte[sizeof(CommandMemBlock) + memSize];
	auto* memBlock = new(buf) CommandMemBlock;
	memBlock->size = memSize;
	memBlock->next = next;
	return *memBlock;
}

std::byte* allocate(CommandRecord& rec, size_t size, unsigned alignment) {
	dlg_assert(alignment <= size);
	dlg_assert((alignment & (alignment - 1)) == 0); // alignment must be power of two
	dlg_assert(alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__); // can't guarantee otherwise

	// We grow block sizes exponentially, up to a maximum
	constexpr auto minBlockSize = 16 * 1024;
	constexpr auto maxBlockSize = 16 * 1024 * 1024;
	constexpr auto blockGrowFac = 2;

	// fast path: enough memory available directly inside the command buffer,
	// simply align and advance the offset
	auto newBlockSize = size_t(minBlockSize);
	if(rec.memBlocks) {
		rec.memBlockOffset = align(rec.memBlockOffset, alignment);
		auto remaining = i64(rec.memBlocks->size) - i64(rec.memBlockOffset);
		if(remaining >= i64(size)) {
			auto ret = data(*rec.memBlocks, rec.memBlockOffset);
			rec.memBlockOffset += size;
			return ret;
		}

		newBlockSize = std::min<size_t>(blockGrowFac * rec.memBlocks->size, maxBlockSize);
	}

	// not enough memory available in last block, allocate new one
	newBlockSize = std::max<size_t>(newBlockSize, size);
	rec.memBlocks.reset(&createMemBlock(newBlockSize, rec.memBlocks.release()));
	rec.memBlockOffset = size;
	return data(*rec.memBlocks, 0);
}

std::byte* allocate(CommandBuffer& cb, size_t size, unsigned alignment) {
	dlg_assert(cb.state() == CommandBuffer::State::recording);
	return allocate(*cb.record(), size, alignment);
}

void copyChainInPlace(CommandBuffer& cb, const void*& pNext) {
	VkBaseInStructure* last = nullptr;
	auto it = pNext;
	while(it) {
		auto src = static_cast<const VkBaseInStructure*>(pNext);
		auto size = structSize(src->sType);
		dlg_assertm(size > 0, "Unknown structure type!");

		auto buf = allocate(cb, size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
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

const void* copyChain(CommandBuffer& cb, const void* pNext) {
	auto ret = pNext;
	copyChainInPlace(cb, ret);
	return ret;
}

} // namespace vil
