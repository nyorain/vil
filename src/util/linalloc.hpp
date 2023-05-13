#pragma once

#include <cstdlib>
#include <vector>
#include <cassert>
#include <cstring>
#include <memory_resource>
#include <functional>
#include <util/allocation.hpp>
#include <util/profiling.hpp>
#include <util/dlg.hpp>
#include <nytl/span.hpp>

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

inline const std::byte* dataBegin(const LinMemBlock& block) {
	return reinterpret_cast<const std::byte*>(&block) + sizeof(LinMemBlock);
}

inline std::byte* dataBegin(LinMemBlock& block) {
	return reinterpret_cast<std::byte*>(&block) + sizeof(LinMemBlock);
}

inline std::size_t memSize(const LinMemBlock& block) {
	return block.end - dataBegin(block);
}

inline std::size_t memOffset(const LinMemBlock& block) {
	return block.data - dataBegin(block);
}

template<typename T>
class UniqueSpan : public span<T> {
public:
	using span<T>::span;
	UniqueSpan(const UniqueSpan& rhs) = delete;
	UniqueSpan& operator=(const UniqueSpan& rhs) = delete;
	UniqueSpan(UniqueSpan&& rhs) noexcept = default;
	UniqueSpan& operator=(UniqueSpan&& rhs) noexcept = default;
	~UniqueSpan() {
		std::destroy_n(this->data(), this->size());
	}
};

struct LinAllocator {
	// We grow block sizes exponentially, up to a maximum
	// NOTE: temporarily increased minBlockSize as it has a huge performance
	// impact on windows, with rdr2 new can take >10ms when we allocate often :(
	// static constexpr auto minBlockSize = 16 * 1024;
	// static constexpr auto maxBlockSize = 16 * 1024 * 1024;
	static constexpr auto minBlockSize = 1024 * 1024;
	static constexpr auto maxBlockSize = minBlockSize;
	static constexpr auto blockGrowFac = 2;

	LinMemBlock memRoot {}; // empty block
	LinMemBlock* memCurrent;

	// NOTE: should be removed later in final release mode.
	// For keeping track of allocation size.
	using Callback = std::function<void(const std::byte*, u32)>;
	Callback onAlloc;
	Callback onFree;

	LinAllocator();
	LinAllocator(Callback alloc, Callback free);
	~LinAllocator();

	// NOTE: could be implemented but need special handling of memRoot
	LinAllocator(LinAllocator&& rhs) noexcept = delete;
	LinAllocator& operator=(LinAllocator&& rhs) noexcept = delete;

	// Resets the allocator to the beginning but does not free any
	// associated memory.
	void reset();

	// Releases all allocated memory
	void release();

	// Returns whether there are no allocations in the allocator.
	bool empty() const;

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
	inline std::byte* allocate(std::size_t size,
			std::size_t alignment) {
		ExtZoneScoped;
		dlg_assert(memCurrent); // there is always a current block

		// fast path (1): enough memory available directly inside the block,
		// simply align and advance the offset
		std::byte* data;
		if(attemptAlloc(*memCurrent, size, alignment, data)) VIL_LIKELY {
			return data;
		}

		// fast path (2): enough memory available in the next block, allocate
		// from there and set it as new block.
		if(memCurrent->next) VIL_LIKELY {
			auto& next = *memCurrent->next;
			// We have to reset it here in case it wasn't reset properly.
			// NOTE: this seems a bit hackish, maybe we can change the design
			// to avoid this here?
			next.data = dataBegin(next);
			if(attemptAlloc(next, size, alignment, data)) VIL_LIKELY {
				memCurrent = &next;
				return data;
			}
		}

		// slow path: we need to allocate a new block
		return addBlock(size, alignment);
	}

	template<typename T, typename... Args>
	[[nodiscard]] T& construct(Args&&... args) {
		auto* raw = allocate(sizeof(T), alignof(T));
		return *new(raw) T(std::forward<Args>(args)...);
	}

	template<typename T>
	span<T> alloc(size_t n) {
		auto ptr = allocRaw<T>(n);
		return {ptr, n};
	}

	template<typename T>
	UniqueSpan<T> allocNonTrivial(size_t n) {
		auto ptr = allocRaw<T, true>(n);
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
		if(n == 0) {
			return {};
		}

		auto ret = this->allocUndef<std::remove_const_t<T>>(n);
		std::memcpy(ret.data(), data, n * sizeof(T));
		return ret;
	}

	template<typename T>
	span<std::remove_const_t<T>> copy(span<T> src) {
		return copy(src.data(), src.size());
	}

	// NOTE: prefer alloc, returning a span.
	// This function be useful for single allocations though.
	template<typename T, bool allowNonTrivial = false>
	T* allocRaw(size_t n = 1) {
		if(n == 0) {
			return nullptr;
		}

		static_assert(allowNonTrivial || std::is_trivially_destructible_v<T>);
		auto ptr = reinterpret_cast<T*>(allocate(sizeof(T) * n, alignof(T)));
		new(ptr) T[n]();
		return ptr;
	}

	// Like allocRaw but does not value-initialize, so may be faster but
	// leaves primitives with undefined values.
	template<typename T, bool allowNonTrivial = false>
	T* allocRawUndef(size_t n = 1) {
		if(n == 0) {
			return nullptr;
		}

		static_assert(allowNonTrivial || std::is_trivially_destructible_v<T>);
		auto ptr = reinterpret_cast<T*>(allocate(sizeof(T) * n, alignof(T)));
		new(ptr) T[n];
		return ptr;
	}

	// own util
	std::byte* addBlock(std::size_t size, std::size_t alignment);
};

// Allocates memory from LinAllocator in a scoped manner.
// Will simply release all allocated memory by resetting the allocation offset
// in the associated LinAllocator when this object is destroyed.
// Alloc calls to different LinAllocScope objects of one LinAllocator must not
// be incorrectly mixed (only allowed in a stack-like manner where memory is
// always only allocated from the last-constructed LinAllocScope that
// wasn't destroyed yet).
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
	// LinAllocScope objects for one LinAllocator at the same time in any
	// way not resembling a stack.
	VIL_DEBUG_ONLY(
		std::byte* current {};
	)

	template<typename T, typename... Args>
	[[nodiscard]] T& construct(Args&&... args) {
		static_assert(std::is_trivially_destructible_v<T>);
		auto* raw = allocBytes(sizeof(T), alignof(T));
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
		if(n == 0u) {
			return {};
		}

		auto ret = this->allocUndef<std::remove_const_t<T>>(n);
		std::memcpy(ret.data(), data, n * sizeof(T));
		return ret;
	}

	// NOTE: prefer alloc, returning a span.
	// This function be useful for single allocations though.
	template<typename T>
	T* allocRaw(size_t n = 1) {
		if(n == 0) {
			return nullptr;
		}

		static_assert(std::is_trivially_destructible_v<T>);
		auto ptr = reinterpret_cast<T*>(allocBytes(sizeof(T) * n, alignof(T)));
		new(ptr) T[n]();
		return ptr;
	}

	// Like allocRaw but does not value-initialize, so may be faster but
	// leaves primitives with undefined values.
	template<typename T>
	T* allocRawUndef(size_t n = 1) {
		if(n == 0) {
			return nullptr;
		}

		static_assert(std::is_trivially_destructible_v<T>);
		auto ptr = reinterpret_cast<T*>(allocBytes(sizeof(T) * n, alignof(T)));
		new(ptr) T[n];
		return ptr;
	}

	template<typename T>
	span<std::remove_const_t<T>> copy(span<T> src) {
		return copy(src.data(), src.size());
	}

	inline std::byte* allocBytes(std::size_t size, std::size_t alignment) {
		VIL_DEBUG_ONLY(
			dlg_assertm(tc.memCurrent->data == this->current,
				"Invalid non-stacking interleaving of LinAllocScope detected");
		)

		auto* ptr = tc.allocate(size, alignment);

		VIL_DEBUG_ONLY(
			current = tc.memCurrent->data;
		)

		return ptr;
	}

	// Only relevant for debugging asserts.
	// When the caller mixes custom usage of the linear allocator
	// with LinAllocScope (not recommended; keep in mind that all custom
	// allocated must not be accessed anymore after the LinAllocScope was
	// destroyed) they can call this function after custom usage
	// to avoid debugging asserts.
	// Perfer using the more confortable customUse() function below
	inline void updateCustomUse() {
		VIL_DEBUG_ONLY(
			current = tc.memCurrent->data;
		)
	}

	struct CustomUsageWrapper {
		LinAllocScope& scope;

		~CustomUsageWrapper() { scope.updateCustomUse(); }
		operator LinAllocator&() const { return scope.tc; }
	};

	CustomUsageWrapper customUse() {
		return {*this};
	}

	inline LinAllocScope(LinAllocator& xla) : tc(xla) {
		block = tc.memCurrent;
		savedPtr = block->data;

		VIL_DEBUG_ONLY(
			current = savedPtr;
		)
	}

	inline ~LinAllocScope() {
		VIL_DEBUG_ONLY(
			dlg_assertm(tc.memCurrent->data == this->current,
				"Invalid non-stacking interleaving of LinAllocScope detected");
		)

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
		auto ptr = memScope_->allocBytes(sizeof(T) * n, alignof(T));
		return reinterpret_cast<T*>(ptr);
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
		auto ptr = linalloc_->allocate(sizeof(T) * n, alignof(T));
		return reinterpret_cast<T*>(ptr);
	}

	void deallocate(T*, size_t) const noexcept {
		// no-op
	}
};

inline std::string_view copy(LinAllocator& alloc, std::string_view src) {
	auto copy = alloc.copy(src.data(), src.size());
	return {copy.data(), copy.size()};
}

template<typename T>
using ScopedVector = std::vector<T, LinearScopedAllocator<T>>;

} // namespace vil

