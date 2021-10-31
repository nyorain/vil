#pragma once

// Since command buffer recording can be a bottleneck, we use
// a custom allocator for it that allocates memory in blocks and
// simply advances offsets. We just free all memory together when a
// record object is destroyed, so we don't need any bookkeeping over
// allocations.
// That's why node-based containers should be preferred with this
// allocator, i.e. linked-list over std::vector as each vector
// reallocation would leave memory unused, only to be freed
// when the CommandRecord is destroyed.

// Previously, we would even retrieve the memory blocks from the
// associated CommandPool and return them there on CommandRecord dstruction
// (if the pool was still alive) but this introduces additional complexity,
// threading concerns for just little performance gain.

#include <fwd.hpp>
#include <memory>
#include <string_view>
#include <list>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <util/span.hpp>
#include <util/ext.hpp>
#include <util/util.hpp>
#include <dlg/dlg.hpp>

namespace vil {

struct CommandMemBlock {
	CommandMemBlock(size_t xsize, CommandMemBlock* xnext = nullptr);
	~CommandMemBlock();

	using Clock = std::chrono::steady_clock;

	CommandMemBlock* next {};
	size_t size {};
	Clock::time_point lastUsed {Clock::now()};

	// following 'this' in memory: std::byte block[size];
};

void freeBlocks(CommandMemBlock* head);

struct MemBlocksListDeleter {
	void operator()(CommandMemBlock* head) const {
		freeBlocks(head);
	}
};

using MemBlockListPtr = std::unique_ptr<CommandMemBlock, MemBlocksListDeleter>;

struct CommandRecordMemory {
	// We own those mem blocks, could even own them past command pool destruction.
	// Important this is the last object to be destroyed as other destructors
	// might still access that memory we free in this destructor.
	MemBlockListPtr memBlocks {};
	u32 memBlockOffset {}; // offset in first (current) mem block
};

inline std::byte* data(CommandMemBlock& mem, size_t offset) {
	constexpr auto alignedStructSize = align(sizeof(CommandMemBlock), __STDCPP_DEFAULT_NEW_ALIGNMENT__);
	dlg_assert(offset < mem.size);
	return reinterpret_cast<std::byte*>(&mem) + alignedStructSize + offset;
}

CommandMemBlock& getCommandMemBlock(std::size_t newBlockSize);

// We really want this function to be inlined
[[nodiscard]] inline std::byte* allocate(MemBlockListPtr& blocks, u32& offset,
		size_t size, unsigned alignment) {
	dlg_assert(alignment <= size);
	dlg_assert((alignment & (alignment - 1)) == 0); // alignment must be power of two
	dlg_assert(alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__); // can't guarantee otherwise

	// We grow block sizes exponentially, up to a maximum
	constexpr auto minBlockSize = 16 * 1024;
	constexpr auto blockGrowFac = 2;

	// fast path: enough memory available directly inside the command buffer,
	// simply align and advance the offset
	auto newBlockSize = size_t(minBlockSize);
	if(blocks) {
		offset = align(offset, alignment);
		if(offset + size <= blocks->size) {
			auto ret = data(*blocks, offset);
			offset += size;
			return ret;
		}

		newBlockSize = blockGrowFac * blocks->size;
	}

	newBlockSize = std::max(newBlockSize, size);
	auto& newBlock = getCommandMemBlock(newBlockSize);

	newBlock.next = blocks.release();
	blocks.reset(&newBlock);

	offset = size;
	return data(newBlock, 0);
}

template<typename T, typename... Args>
[[nodiscard]] T& allocate(CommandRecordMemory& rec, Args&&... args) {
	auto* raw = allocate(rec.memBlocks, rec.memBlockOffset, sizeof(T), alignof(T));
	return *(new(raw) T(std::forward<Args>(args)...));
}

template<typename T>
[[nodiscard]] span<T> allocSpan(CommandRecordMemory& rec, size_t count) {
	if(count == 0) {
		return {};
	}

	auto* raw = allocate(rec.memBlocks, rec.memBlockOffset, sizeof(T) * count, alignof(T));
	auto* arr = new(raw) T[count];
	return span<T>(arr, count);
}

template<typename T>
[[nodiscard]] span<std::remove_const_t<T>> copySpan(CommandRecordMemory& rec, T* data, size_t count) {
	if(count == 0u) {
		return {};
	}

	auto span = allocSpan<std::remove_const_t<T>>(rec, count);
	std::copy(data, data + count, span.data());
	return span;
}

template<typename T>
[[nodiscard]] span<std::remove_const_t<T>> copySpan(CommandRecordMemory& rec, span<T> data) {
	return copySpan(rec, data.data(), data.size());
}

inline const char* copyString(CommandRecordMemory& rec, std::string_view src) {
	auto dst = allocSpan<char>(rec, src.size() + 1);
	std::copy(src.begin(), src.end(), dst.data());
	dst[src.size()] = 0;
	return dst.data();
}

// Allocator implementation, e.g. for stl containers that allocates from
// a stored CommandRecord.
template<typename T>
struct CommandAllocator {
	using is_always_equal = std::false_type;
	using value_type = T;

	CommandRecordMemory* rec;

	CommandAllocator(CommandRecordMemory& xrec) noexcept : rec(&xrec) {}

	template<typename O>
	CommandAllocator(const CommandAllocator<O>& rhs) noexcept : rec(rhs.rec) {}

	template<typename O>
	CommandAllocator& operator=(const CommandAllocator<O>& rhs) noexcept {
		this->rec = rhs.rec;
		return *this;
	}

	T* allocate(size_t n) {
		auto* ptr = vil::allocate(rec->memBlocks, rec->memBlockOffset, n * sizeof(T), alignof(T));
        // strictly speaking we need the first line but it doesn't compile
        // under msvc for non-default-constructible T
		// return new(ptr) T[n]; // creates the array but not the objects
        return reinterpret_cast<T*>(ptr);
	}

	void deallocate(T*, size_t) const noexcept {
		// nothing to do here, we never deallocate individual allocations,
		// we'll just all the blocks in the CommandRecord when it's destroyed.
	}
};

template<typename T>
bool operator==(const CommandAllocator<T>& a, const CommandAllocator<T>& b) noexcept {
	return a.rec == b.rec;
}

template<typename T>
bool operator!=(const CommandAllocator<T>& a, const CommandAllocator<T>& b) noexcept {
	return a.rec != b.rec;
}

template<typename T> using CommandAllocList = std::list<T, CommandAllocator<T>>;
template<typename K, typename V> using CommandAllocHashMap =
	std::unordered_map<K, V,
		std::hash<K>,
		std::equal_to<K>,
		CommandAllocator<std::pair<const K, V>>>;
template<typename K> using CommandAllocHashSet =
	std::unordered_set<K,
		std::hash<K>,
		std::equal_to<K>,
		CommandAllocator<K>>;

} // namespace vil
