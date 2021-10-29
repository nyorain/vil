#include <command/alloc.hpp>
#include <util/util.hpp>
#include <util/profiling.hpp>
#include <cb.hpp>
#include <chrono>

namespace vil {

constexpr auto alignedStructSize = align(sizeof(CommandMemBlock), __STDCPP_DEFAULT_NEW_ALIGNMENT__);

CommandMemBlock::CommandMemBlock(size_t xsize, CommandMemBlock* xnext)
		: next(xnext), size(xsize) {
	auto memSize = size + alignedStructSize;
	TracyAllocS(this, sizeof(*this) + size, 8);
	DebugStats::get().commandMem += memSize;
}

CommandMemBlock::~CommandMemBlock() {
	auto memSize = size + alignedStructSize;
	TracyFreeS(this, 8);
	DebugStats::get().commandMem -= memSize;
}

CommandMemBlock& createMemBlock(size_t memSize, CommandMemBlock* next = nullptr) {
	ZoneScoped;
	auto buf = new std::byte[memSize];
	auto* memBlock = new(buf) CommandMemBlock(memSize - alignedStructSize, next);
	return *memBlock;
}

// WIP
struct MemBlockList {
	std::vector<CommandMemBlock*> blocks {};
	std::mutex mutex;

	// using Clock = CommandMemBlock::Clock;
	// Clock::time_point lastCheckedUnused {Clock::now()};
};

CommandMemBlock* getBlock(MemBlockList& list) {
	std::lock_guard lock(list.mutex);
	if(list.blocks.empty()) {
		return nullptr;
	}

	auto* ret = list.blocks.back();
	list.blocks.pop_back();
	return ret;
}

void freeUnused(MemBlockList& list) {
	std::lock_guard lock(list.mutex);

	constexpr auto freeThreshold = std::chrono::seconds(2u);
	auto now = CommandMemBlock::Clock::now();

	auto it = list.blocks.begin();
	for(; it != list.blocks.end(); ++it) {
		auto& block = **it;

		// The list is ordered by last used, with the oldest blocks
		// coming first. Once we reach a block that has to be preserved,
		// we can break.
		if(now - block.lastUsed < freeThreshold) {
			break;
		}

		block.~CommandMemBlock();
		delete[] reinterpret_cast<std::byte*>(&block);
	}

	if(it != list.blocks.begin()) {
		list.blocks.erase(list.blocks.begin(), it);
	}
}

void returnBlock(MemBlockList& list, CommandMemBlock& block) {
	std::lock_guard lock(list.mutex);
	block.next = nullptr;
	list.blocks.push_back(&block);
}

struct MemBlockManager {
	static constexpr std::array sizes = {
		64 * 1024u, 		// up to 64 KB
		512 * 1024 * 1024u, // up to 512 KB
		4 * 1024 * 1024u, 	// up to 4MB
		16 * 1024 * 1024u, 	// up to 16 MB
	};

	std::array<MemBlockList, sizes.size()> lists;
};

CommandMemBlock& getBlock(MemBlockManager& manager, size_t size) {
	auto cat = 0xFFu;
	for(auto i = 0u; i < manager.sizes.size(); ++i) {
		if(manager.sizes[i] > size) {
			cat = i;
			break;
		}
	}

	if(cat == 0xFFu) {
		// Someone requested an allocation > 16MB. We don't ever cache those.
		dlg_warn("Extremely large command mem block required: {}", size);
		return createMemBlock(size, nullptr);
	}

	auto ret = getBlock(manager.lists[cat]);
	if(ret) {
		return *ret;
	}

	return createMemBlock(size);
}

void returnBlock(MemBlockManager& manager, CommandMemBlock& block) {
	if(block.size > manager.sizes.back()) {
		// HUGE mem block, we don't want to preserve it
		delete &block;
		return;
	}

	auto cat = 0xFFu;
	for(auto i = 0u; i < manager.sizes.size(); ++i) {
		if(manager.sizes[i] > block.size) {
			cat = i;
			break;
		}
	}

	dlg_assert(cat < manager.lists.size());
	block.lastUsed = CommandMemBlock::Clock::now();
	returnBlock(manager.lists[cat], block);

	for(auto& list : manager.lists) {
		freeUnused(list);
	}
}

// alloc/free implementation
// #define USE_GLOBAL_MEMBLOCKS

MemBlockManager& globalMemBlocks() {
	static MemBlockManager ret;
	return ret;
}

CommandMemBlock& getCommandMemBlock(std::size_t newBlockSize) {
#ifdef USE_GLOBAL_MEMBLOCKS
	return getBlock(globalMemBlocks(), newBlockSize);
#else // USE_GLOBAL_MEMBLOCKS
	return createMemBlock(newBlockSize, nullptr);
#endif // USE_GLOBAL_MEMBLOCKS
}

/*
std::byte* allocate(CommandRecord& rec, size_t size, unsigned alignment) {
	dlg_assert(alignment <= size);
	dlg_assert((alignment & (alignment - 1)) == 0); // alignment must be power of two
	dlg_assert(alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__); // can't guarantee otherwise

	// We grow block sizes exponentially, up to a maximum
	constexpr auto minBlockSize = 16 * 1024;
	constexpr auto maxBlockSize = MemBlockManager::sizes.back();
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
	newBlockSize = std::max<size_t>(newBlockSize, size + alignedStructSize);

#ifdef USE_GLOBAL_MEMBLOCKS
	auto& newBlock = getBlock(globalMemBlocks(), newBlockSize);
#else // USE_GLOBAL_MEMBLOCKS
	auto& newBlock = createMemBlock(newBlockSize, nullptr);
#endif // USE_GLOBAL_MEMBLOCKS

	newBlock.next = rec.memBlocks.release();
	rec.memBlocks.reset(&newBlock);

	rec.memBlockOffset = size;
	return data(*rec.memBlocks, 0);
}
*/

void freeBlocks(CommandMemBlock* head) {
	ZoneScoped;

	if(!head) {
		return;
	}

	// Free all memory blocks
	while(head) {
		auto next = head->next;

#ifdef USE_GLOBAL_MEMBLOCKS
		returnBlock(globalMemBlocks(), *head);
#else // USE_GLOBAL_MEMBLOCKS
		head->~CommandMemBlock();
		delete[] reinterpret_cast<std::byte*>(head);
#endif // USE_GLOBAL_MEMBLOCKS

		head = next;
	}
}

} // namespace vil
