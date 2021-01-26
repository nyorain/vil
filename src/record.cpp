#include <record.hpp>
#include <commands.hpp>
#include <image.hpp>
#include <pipe.hpp>
#include <cb.hpp>
#include <util/util.hpp>
#include <gui/commandHook.hpp>

namespace vil {

std::byte* data(CommandMemBlock& mem, std::size_t offset) {
	dlg_assert(offset < mem.size);
	return reinterpret_cast<std::byte*>(&mem) + sizeof(mem) + offset;
}

CommandMemBlock& createMemBlock(std::size_t memSize, CommandMemBlock* next) {
	auto buf = new std::byte[sizeof(CommandMemBlock) + memSize];
	auto* memBlock = new(buf) CommandMemBlock;
	memBlock->size = memSize;
	memBlock->next = next;
	return *memBlock;
}

CommandMemBlock& fetchMemBlock(CommandPool& pool, std::size_t size, CommandMemBlock* bnext) {
	// NOTE: could search all mem blocks for one large enough.
	// On the other hand, allocations larger than block size are unlikely
	// so it's probably best to just do a relatively quick one-block check.
	if(pool.memBlocks) {
		auto* block = pool.memBlocks.load();
		auto* next = block->next.load();
		while(!pool.memBlocks.compare_exchange_weak(block, next)) {
			next = block->next.load();
		}

		block->next = bnext;
		if(block->size >= size) {
			return *block;
		}

		// Otherwise we have to allocate a new block after all.
		// Instead of returning the wrongly allocated (since too small)
		// block, we simply append it, all of the allocated block is used
		// up by this allocation anyways.
		bnext = block;
	}

	// create entirely new block
	auto blockSize = std::max<std::size_t>(CommandPool::minMemBlockSize, size);
	return createMemBlock(blockSize, bnext);
}

std::byte* CommandRecord::allocate(std::size_t size, std::size_t alignment) {
	dlg_assert(alignment <= size);
	dlg_assert((alignment & (alignment - 1)) == 0); // alignment must be power of two
	dlg_assert(alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__); // can't guarantee otherwise

	dlg_assert(this->cb);
	dlg_assert(!this->finished);

	// fast path: enough memory available directly inside the command buffer,
	// simply align and advance the offset
	if(memBlocks.get()) {
		memBlockOffset = align(memBlockOffset, alignment);
		auto remaining = i64(memBlocks->size) - i64(memBlockOffset);
		if(remaining >= i64(size)) {
			auto ret = data(*memBlocks, memBlockOffset);
			memBlockOffset += size;
			return ret;
		}
	}

	auto next = memBlocks.release();
	memBlocks.reset(&fetchMemBlock(cb->pool(), size, next));

	memBlockOffset = size;
	return data(*memBlocks, 0);
}

std::byte* allocate(CommandRecord& rec, std::size_t size, std::size_t alignment) {
	return rec.allocate(size, alignment);
}

std::byte* allocate(CommandBuffer& cb, std::size_t size, std::size_t alignment) {
	dlg_assert(cb.state() == CommandBuffer::State::recording);
	return cb.record()->allocate(size, alignment);
}

void returnBlocks(CommandPool& pool, CommandMemBlock* head) {
	if(!head) {
		return;
	}

	auto tail = head;
	auto next = tail;
	while((next = tail->next.load()) != nullptr) {
		tail = next;
	}

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

// NOTE: We need them to be noexcept. We can't rely on the internal
//   std::list to be noexcept movable though. So we implement it ourselves,
//   effectively intentionally crashing the program (throwing from noexcept)
//   when an stl throws from the move operation.
//   Could be fixed later on with our own linked-list/grid.
UsedImage::UsedImage(UsedImage&& rhs) noexcept :
		image(rhs.image),
		layoutChanged(rhs.layoutChanged),
		finalLayout(rhs.finalLayout),
		commands(std::move(rhs.commands)) {
}

UsedImage& UsedImage::operator=(UsedImage&& rhs) noexcept {
	image = rhs.image;
	layoutChanged = rhs.layoutChanged;
	finalLayout = rhs.finalLayout;
	commands = std::move(rhs.commands);
	return *this;
}

UsedHandle::UsedHandle(UsedHandle&& rhs) noexcept :
		handle(rhs.handle),
		commands(std::move(rhs.commands)) {
}

UsedHandle& UsedHandle::operator=(UsedHandle&& rhs) noexcept {
	handle = rhs.handle;
	commands = std::move(rhs.commands);
	return *this;
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
		queueFamily(xcb.pool().queueFamily), images(*this), handles(*this),
		pipeLayouts(*this), secondaries(*this) {
}

CommandRecord::~CommandRecord() {
	auto dev = memBlocks.get_deleter().dev;
	if(!dev) {
		return;
	}

	std::lock_guard lock(dev->mutex);

	if(group) {
		auto count = group->aliveRecords.erase(this);
		dlg_assertm(count, "CommandRecord not found in its group");
	}

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

	// Just to be safe, its destructor might reference this.
	// And must be called while mutex is locked.
	hook.reset();
}

void unsetDestroyedLocked(CommandRecord& record) {
	if(record.destroyed.empty()) {
		return;
	}

	// unset in commands
	// NOTE: we could query commands where handles are used via usedHandles
	// maps. Might give speedup for large command buffers. But introduces
	// new complexity and problems, maybe not worth it.
	// Same optimization below when removing from usedHandles.
	// Would need the raw vulkan handle though, we don't access to that
	// here anyways at the moment. But that should be doable if really
	// needed, might be good idea to move the usedHandles maps to use
	// our Handles (i.e. Image*) as key anyways.
	auto* cmd = record.commands;
	while(cmd) {
		cmd->unset(record.destroyed);
		cmd = cmd->next;
	}

	// remove from handles
	for(auto it = record.handles.begin(); it != record.handles.end(); ) {
		if(record.destroyed.count(it->second.handle)) {
			it = record.handles.erase(it);
		} else {
			++it;
		}
	}

	for(auto it = record.images.begin(); it != record.images.end(); ) {
		if(record.destroyed.count(it->second.image)) {
			it = record.images.erase(it);
		} else {
			++it;
		}
	}

	record.destroyed.clear();
}

} // namespace vil
