#pragma once

#include <fwd.hpp>
#include <cstdlib>
#include <vector>
#include <cassert>
#include <memory_resource>
#include <util/util.hpp>
#include <util/profiling.hpp>
#include <dlg/dlg.hpp>

// per-thread allocator for temporary local memory, allocated in stack-like
// fashion. Due to its strict requirement, only useful to create one-shot
// continuous sequences. When used e.g. for vector, does not allow resizing.
// because we don't know the size.

namespace vil {

struct ThreadMemBlock {
	ThreadMemBlock* prev {};
	ThreadMemBlock* next {};
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

inline std::size_t memSize(const ThreadMemBlock& block) {
	return block.end - (reinterpret_cast<const std::byte*>(&block) + sizeof(ThreadMemBlock));
}

inline std::size_t memOffset(const ThreadMemBlock& block) {
	return block.data - (reinterpret_cast<const std::byte*>(&block) + sizeof(ThreadMemBlock));
}

// All data we need per-thread. Currently only used for stack-like
// dynamic memory allocation.
struct ThreadContext {
	// NOTE: C++ does not clearly specify when its constructor will
	// be called, just that it's before it's used the first time.
	// This means we might be creating them for *every* thread even
	// if its never used there. Keep this in mind when increasing the
	// minBlockSize.
	static thread_local ThreadContext instance;

	// We grow block sizes exponentially, up to a maximum
	static constexpr auto minBlockSize = 16 * 1024;
	static constexpr auto maxBlockSize = 16 * 1024 * 1024;
	static constexpr auto blockGrowFac = 2;

	ThreadMemBlock* memRoot {};
	ThreadMemBlock* memCurrent {};

	ThreadContext();
	~ThreadContext();
};

void freeBlocks(ThreadMemBlock* head);
std::byte* addBlock(ThreadContext& tc, std::size_t size, std::size_t alignment);

// We really want this function to be inlined (in release mode at least)
// so we keep it as small as possible.
inline std::byte* attemptAlloc(ThreadMemBlock& block, std::size_t size,
		std::size_t alignment) {
	VIL_DEBUG_ONLY(dlg_assert(block.canary == ThreadMemBlock::canaryValue));
	dlg_assert(block.data <= block.end);

	auto dataUint = reinterpret_cast<std::uintptr_t>(block.data);
	auto alignedData = alignPOT(dataUint, alignment);
	auto allocBegin = reinterpret_cast<std::byte*>(alignedData);
	auto allocEnd = allocBegin + size;

	if(allocEnd > block.end) VIL_UNLIKELY {
		return nullptr;
	}

	block.data = allocEnd;
	return allocBegin;
}

// We really want this function to be inlined (at least in release
// with asserts and debug checks disabled).
inline std::byte* allocate(ThreadContext& tc, std::size_t size,
		std::size_t alignment) {
	ExtZoneScoped;
	dlg_assert(tc.memCurrent); // there is always a current block

	// fast path (1): enough memory available directly inside the block,
	// simply align and advance the offset
	if(auto* data = attemptAlloc(*tc.memCurrent, size, alignment); data) VIL_LIKELY {
		return data;
	}

	// fast path (2): enough memory available in the next block, allocate
	// from there and set it as new block.
	if(tc.memCurrent->next) VIL_LIKELY {
		auto& next = *tc.memCurrent->next;
		dlg_assert(memOffset(next) == 0u);
		if(auto* data = attemptAlloc(next, size, alignment); data) VIL_LIKELY {
			tc.memCurrent = &next;
			return data;
		}
	}

	// slow path: we need to allocate a new block
	return addBlock(tc, size, alignment);
}

// Allocated memory from ThreadContext.
// Will simply release all allocated memory by resetting the allocation offset
// in the current ThreadContext when this object is destroyed.
// The object must therefore not be used/moved across thread boundaries or
// alloc to different ThreadMemScope objects be incorrectly mixed (only allowed
// in a stack-like manner).
// When this is used, it must be the only mechanism by which memory
// from the thread context is allocated.
// NOTE: for best performance, we try to allow the compiler to inline the
// fast path for the alloc functions below, effectively reducing an alloc
// call to just a couple of instructions.
struct ThreadMemScope {
	ThreadMemBlock* block {};
	std::byte* savedPtr {}; // the saved offset

	// only for debugging, making sure that we never use multiple
	// ThreadMemScope objects at the same time in any way not resembling
	// a stack.
	VIL_DEBUG_ONLY(
		std::byte* current {};
	)

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
		auto ret = this->alloc<std::remove_const_t<T>>(n);
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
		auto& tc = ThreadContext::instance;

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

	inline ThreadMemScope() {
		auto& tc = ThreadContext::instance;
		block = tc.memCurrent;
		savedPtr = block->data;

		VIL_DEBUG_ONLY(
			current = savedPtr;
		)
	}

	inline ~ThreadMemScope() {
		auto& tc = ThreadContext::instance;
		tc.memCurrent = block;
		tc.memCurrent->data = savedPtr;
	}
};

template<typename T>
struct ThreadMemoryAllocator {
	using is_always_equal = std::false_type;
	using value_type = T;

	ThreadMemScope* memScope_;

	ThreadMemoryAllocator(ThreadMemScope& tms) noexcept : memScope_(&tms) {}

	template<typename O>
	ThreadMemoryAllocator(const ThreadMemoryAllocator<O>& rhs) noexcept :
		memScope_(rhs.memScope_) {}

	template<typename O>
	ThreadMemoryAllocator& operator=(const ThreadMemoryAllocator<O>& rhs) noexcept {
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

class ThreadMemoryResource : public std::pmr::memory_resource {
	ThreadMemScope* memScope_ {};

	void* do_allocate(std::size_t bytes, std::size_t alignment) override {
		return memScope_->allocBytes(bytes, alignment);
	}

	void do_deallocate(void*, std::size_t, std::size_t) override {
		// no-op
	}

	bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
		auto* tmr = dynamic_cast<const ThreadMemoryResource*>(&other);
		if(!tmr) {
			return false;
		}

		return tmr->memScope_ == this->memScope_;
	}
};

} // namespace vil
