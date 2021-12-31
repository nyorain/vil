#pragma once

#include <cstdlib>
#include <vector>
#include <cassert>
#include <cstring>
#include <memory_resource>
#include <util/util.hpp>
#include <util/profiling.hpp>
#include <dlg/dlg.hpp>

// Simple but optimized linear allocator implementation
// NOTE: Take care modifying this code in future, it was optimized so that
// the allocation fast path only needs ~6 instructions (1 load, 1 store).
// Creating a LinAllocScope has ~7 instructions with ~2 independent loads.
// See node 2107.
// TODO: add support for retrieving the memory blocks from a parent
// allocator instead of calling new[], delete[] directly every time.
// PERF: maybe don't support any alignment? Instead define a
// maxAlignment and always align allocation size to multiple? We could
// hope that constant folding will detect that object size is a multiple
// in most cases (hm but this would only work for maxAlignment = sizeof(void*),
// is that really enough for all cases?) and completely remove the align
// computation

namespace vil {

// Memory block of the linear allocator. The blocks form a forward-linked list.
struct LinMemBlock {
	LinMemBlock* next {};
	std::byte* data {};
	std::byte* end {};

	VIL_DEBUG_ONLY(
		// since we store the metadata right next to the raw byte array, we
		// use a canary in debugging to warn about possibly corrupt metadat
		// as early as possible.
		static constexpr auto canaryValue = u64(0xCAFEC0DED00DDEADULL);
		u64 canary {canaryValue};
	)

	// following: std::byte[]
};

inline std::size_t memSize(const LinMemBlock& block) {
	return block.end - (reinterpret_cast<const std::byte*>(&block) + sizeof(LinMemBlock));
}

inline std::size_t memOffset(const LinMemBlock& block) {
	return block.data - (reinterpret_cast<const std::byte*>(&block) + sizeof(LinMemBlock));
}

struct LinAllocator {
	// We grow block sizes exponentially, up to a maximum
	static constexpr auto minBlockSize = 16 * 1024;
	static constexpr auto maxBlockSize = 16 * 1024 * 1024;
	static constexpr auto blockGrowFac = 2;

	LinMemBlock* memRoot;
	LinMemBlock* memCurrent;

	LinAllocator();
	~LinAllocator();
};

void freeBlocks(LinMemBlock* head);
std::byte* addBlock(LinAllocator& tc, std::size_t size, std::size_t alignment);

// We really want this function to be inlined (in release mode at least)
// so we keep it as small as possible.
inline bool attemptAlloc(LinMemBlock& block, std::size_t size,
		std::size_t alignment, std::byte*& ret) {
	VIL_DEBUG_ONLY(dlg_assert(block.canary == LinMemBlock::canaryValue));
	dlg_assert(block.data <= block.end);

	auto dataUint = reinterpret_cast<std::uintptr_t>(block.data);
	auto alignedData = alignPOT(dataUint, alignment);
	auto allocBegin = reinterpret_cast<std::byte*>(alignedData);
	auto allocEnd = allocBegin + size;

	if(allocEnd > block.end) VIL_UNLIKELY {
		return false;
	}

	block.data = allocEnd;
	ret = allocBegin;
    return true;
}

// We really want this function to be inlined (at least in release
// with asserts and debug checks disabled).
inline std::byte* allocate(LinAllocator& tc, std::size_t size,
		std::size_t alignment) {
	ExtZoneScoped;
	dlg_assert(tc.memCurrent); // there is always a current block

	// fast path (1): enough memory available directly inside the block,
	// simply align and advance the offset
    std::byte* data;
	if(attemptAlloc(*tc.memCurrent, size, alignment, data)) VIL_LIKELY {
		return data;
	}

	// fast path (2): enough memory available in the next block, allocate
	// from there and set it as new block.
	if(tc.memCurrent->next) VIL_LIKELY {
		auto& next = *tc.memCurrent->next;
		dlg_assert(memOffset(next) == 0u);
		if(attemptAlloc(next, size, alignment, data)) VIL_LIKELY {
			tc.memCurrent = &next;
			return data;
		}
	}

	// slow path: we need to allocate a new block
	return addBlock(tc, size, alignment);
}

// Allocates memory from LinearAllocator in a scoped manner.
// Will simply release all allocated memory by resetting the allocation offset
// in the current ThreadContext when this object is destroyed.
// Alloc calls to different ThreadMemScope objects must not be incorrectly
// mixed (only allowed in a stack-like manner where memory is always only
// allocated from the last-constructed LinAllocScope).
// When this is used, it must be the only mechanism by which memory
// from the linear allocator is allocated. There are debug checks in place
// making sure this is done correctly.
// NOTE: for best performance, we try to allow the compiler to inline the
// fast path for the alloc functions below, effectively reducing an alloc
// call to just a couple of instructions.
struct LinAllocScope {
	LinMemBlock* block; // the block saved during construction
	std::byte* savedPtr; // the offset saved during construction

	// NOTE: storing this here is an optimization for compilers that
	// lazy-initialize thread local storage.
	LinAllocator& tc;

	// only for debugging, making sure that we never use multiple
	// ThreadMemScope objects at the same time in any way not resembling
	// a stack.
	VIL_DEBUG_ONLY(
		std::byte* current {};
	)

	template<typename T, typename... Args>
	[[nodiscard]] T& construct(Args&&... args) {
		auto* raw = vil::allocate(tc, sizeof(T), alignof(T));
		return *new(raw) T(std::forward<Args>(args)...);
	}

	template<typename T>
	span<T> alloc(size_t n) {
		auto ptr = allocRaw<T>(n);
		return {ptr, n};
	}

	// Like alloc but does not value-initialize, so may be faster but
	// leaves primitives with undefined values.
	template<typename T>
	span<T> allocUndef(size_t n) {
		auto ptr = allocRawUndef<T>(n);
		return {ptr, n};
	}

	template<typename T>
	span<std::remove_const_t<T>> copy(T* data, size_t n) {
		auto ret = this->allocUndef<std::remove_const_t<T>>(n);
		std::memcpy(ret.data(), data, n * sizeof(T));
		return ret;
	}

	// NOTE: prefer alloc, returning a span.
	// This function be useful for single allocations though.
	template<typename T>
	T* allocRaw(size_t n = 1) {
		auto ptr = reinterpret_cast<T*>(allocBytes(sizeof(T) * n, alignof(T)));
		new(ptr) T[n]();
		return ptr;
	}

	// Like allocRaw but does not value-initialize, so may be faster but
	// leaves primitives with undefined values.
	template<typename T>
	T* allocRawUndef(size_t n = 1) {
		auto ptr = reinterpret_cast<T*>(allocBytes(sizeof(T) * n, alignof(T)));
		new(ptr) T[n];
		return ptr;
	}

	inline std::byte* allocBytes(std::size_t size, std::size_t alignment) {
		VIL_DEBUG_ONLY(
			dlg_assertm(tc.memCurrent->data == this->current,
				"Invalid non-stacking interleaving of ThreadMemScope detected");
		)

		auto* ptr = vil::allocate(tc, size, alignment);

		VIL_DEBUG_ONLY(
			current = tc.memCurrent->data;
		)

		return ptr;
	}

	inline LinAllocScope(LinAllocator& xla) : tc(xla) {
		block = tc.memCurrent;
		savedPtr = block->data;

		VIL_DEBUG_ONLY(
			current = savedPtr;
		)
	}

	inline ~LinAllocScope() {
		tc.memCurrent = block;
		tc.memCurrent->data = savedPtr;
	}

	// Doesn't make sense
	LinAllocScope(LinAllocScope&&) noexcept = delete;
	LinAllocScope& operator=(LinAllocScope&&) noexcept = delete;
};

template<typename T>
struct LinearScopedAllocator {
	using is_always_equal = std::false_type;
	using value_type = T;

	LinAllocScope* memScope_;

	LinearScopedAllocator(LinAllocScope& tms) noexcept : memScope_(&tms) {}

	template<typename O>
	LinearScopedAllocator(const LinearScopedAllocator<O>& rhs) noexcept :
		memScope_(rhs.memScope_) {}

	template<typename O>
	LinearScopedAllocator& operator=(const LinearScopedAllocator<O>& rhs) noexcept {
		this->rec = rhs.rec;
		return *this;
	}

	T* allocate(size_t n) {
		return memScope_->allocRaw<T>(n);
	}

	void deallocate(T*, size_t) const noexcept {
		// no-op
	}
};

// Must not be mixed with scoped usage of the LinAllocator object
template<typename T>
struct LinearUnscopedAllocator {
	using is_always_equal = std::false_type;
	using value_type = T;

	LinAllocator* linalloc_;

	LinearUnscopedAllocator(LinAllocator& linalloc) noexcept : linalloc_(&linalloc) {}

	template<typename O>
	LinearUnscopedAllocator(const LinearUnscopedAllocator<O>& rhs) noexcept :
		linalloc_(rhs.linalloc_) {}

	template<typename O>
	LinearUnscopedAllocator& operator=(const LinearUnscopedAllocator<O>& rhs) noexcept {
		this->rec = rhs.rec;
		return *this;
	}

	T* allocate(size_t n) {
		auto raw = vil::allocate(*linalloc_, sizeof(T) * n, alignof(T));
		auto ptr = reinterpret_cast<T*>(raw);
		new(ptr) T[n]();
		return ptr;
	}

	void deallocate(T*, size_t) const noexcept {
		// no-op
	}
};

} // namespace vil

