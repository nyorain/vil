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
#include <unordered_map>
#include <unordered_set>
#include <util/span.hpp>
#include <util/ext.hpp>
#include <dlg/dlg.hpp>

namespace vil {

struct CommandMemBlock {
	CommandMemBlock* next {};
	size_t size {};
	// std::byte block[size]; // following 'this' in memory
};

[[nodiscard]] std::byte* allocate(CommandRecord&, size_t size, unsigned align);
[[nodiscard]] std::byte* allocate(CommandBuffer&, size_t size, unsigned align);

void freeBlocks(CommandMemBlock* head);

struct MemBlocksListDeleter {
	void operator()(CommandMemBlock* head) const {
		freeBlocks(head);
	}
};

template<typename T, typename... Args>
[[nodiscard]] T& allocate(CommandBuffer& cb, Args&&... args) {
	auto* raw = allocate(cb, sizeof(T), alignof(T));
	return *(new(raw) T(std::forward<Args>(args)...));
}

template<typename T>
[[nodiscard]] span<T> allocSpan(CommandBuffer& cb, size_t count) {
	if(count == 0) {
		return {};
	}

	auto* raw = allocate(cb, sizeof(T) * count, alignof(T));
	auto* arr = new(raw) T[count];
	return span<T>(arr, count);
}

template<typename T>
[[nodiscard]] span<T> allocSpan(CommandRecord& rec, size_t count) {
	if(count == 0) {
		return {};
	}

	auto* raw = allocate(rec, sizeof(T) * count, alignof(T));
	auto* arr = new(raw) T[count];
	return span<T>(arr, count);
}

template<typename T>
[[nodiscard]] span<std::remove_const_t<T>> copySpan(CommandBuffer& cb, T* data, size_t count) {
	auto span = allocSpan<std::remove_const_t<T>>(cb, count);
	std::copy(data, data + count, span.data());
	return span;
}

template<typename T>
[[nodiscard]] span<std::remove_const_t<T>> copySpan(CommandBuffer& cb, span<T> data) {
	return copySpan(cb, data.data(), data.size());
}

inline const char* copyString(CommandRecord& rec, std::string_view src) {
	auto dst = allocSpan<char>(rec, src.size() + 1);
	std::copy(src.begin(), src.end(), dst.data());
	dst[src.size()] = 0;
	return dst.data();
}

inline const char* copyString(CommandBuffer& cb, std::string_view src) {
	auto dst = allocSpan<char>(cb, src.size() + 1);
	std::copy(src.begin(), src.end(), dst.data());
	dst[src.size()] = 0;
	return dst.data();
}

template<typename T>
void ensureSize(CommandBuffer& cb, span<T>& buf, std::size_t size) {
	if(buf.size() >= size) {
		return;
	}

	auto newBuf = allocSpan<T>(cb, size);
	std::copy(buf.begin(), buf.end(), newBuf.begin());
	buf = newBuf;
}

template<typename D, typename T>
void upgradeSpan(CommandBuffer& cb, span<D>& dst, T* data, std::size_t count) {
	dst = allocSpan<D>(cb, count);
	for(auto i = 0u; i < count; ++i) {
		dst[i] = upgrade(data[i]);
	}
}

// Allocates the memory from the command buffer.
void copyChainInPlace(CommandBuffer& cb, const void*& pNext);
[[nodiscard]] const void* copyChain(CommandBuffer& cb, const void* pNext);

// Allocator implementation, e.g. for stl containers that allocates from
// a stored CommandRecord.
template<typename T>
struct CommandAllocator {
	using is_always_equal = std::false_type;
	using value_type = T;

	CommandRecord* rec;

	CommandAllocator(CommandRecord& xrec) noexcept : rec(&xrec) {}

	template<typename O>
	CommandAllocator(const CommandAllocator<O>& rhs) noexcept : rec(rhs.rec) {}

	template<typename O>
	CommandAllocator& operator=(const CommandAllocator<O>& rhs) noexcept {
		this->rec = rhs.rec;
		return *this;
	}

	T* allocate(std::size_t n) {
		auto* ptr = vil::allocate(*rec, n * sizeof(T), alignof(T));
        // strictly speaking we need the first line but it doesn't compile
        // under msvc for non-default-constructible T
		// return new(ptr) T[n]; // creates the array but not the objects
        return reinterpret_cast<T*>(ptr);
	}

	void deallocate(T*, std::size_t) const noexcept {
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
