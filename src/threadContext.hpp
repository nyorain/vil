#pragma once

#include <fwd.hpp>
#include <cstdlib>
#include <vector>
#include <cassert>
#include <memory_resource>
#include <util/util.hpp>

// per-thread allocator for temporary local memory, allocated in stack-like
// fashion. Due to its strict requirement, only useful to create one-shot
// continuous sequences. When used e.g. for vector, does not allow resizing.
// because we don't know the size.

namespace vil {

struct ThreadMemBlock {
	ThreadMemBlock* prev {};
	ThreadMemBlock* next {};
	size_t size {};
	size_t offset {};

#ifdef VIL_DEBUG
	// since we store the metadata right next to the raw byte array, we
	// use a canary in debugging to warn about possibly corrupt metadat
	// as early as possible.
	static constexpr auto canaryValue = u64(0xCAFEC0DED00DDEADULL);
	u64 canary {canaryValue};
#endif // VIL_DEBUG

	// following: std::byte[size]
};

// All data we need per-thread. Currently only used for stack-like
// dynamic memory allocation.
struct ThreadContext {
	static ThreadContext& get();

	// We grow block sizes exponentially, up to a maximum
	static constexpr auto minBlockSize = 16 * 1024;
	static constexpr auto maxBlockSize = 16 * 1024 * 1024;
	static constexpr auto blockGrowFac = 2;

	ThreadMemBlock* memRoot {};
	ThreadMemBlock* memCurrent {};

	ThreadContext();
	~ThreadContext();
};

inline std::byte* data(ThreadMemBlock& mem, size_t offset) {
	dlg_assert(offset <= mem.size); // offset == mem.size as 'end' ptr is ok
	constexpr auto objSize = align(sizeof(mem), __STDCPP_DEFAULT_NEW_ALIGNMENT__);
	return reinterpret_cast<std::byte*>(&mem) + objSize + offset;
}

void freeBlocks(ThreadMemBlock* head);
ThreadMemBlock& createMemBlock(size_t memSize, ThreadMemBlock* prev);

// We really want this function to be inlined
inline std::byte* allocate(ThreadContext& tc, size_t size) {
	ExtZoneScoped;

	dlg_assert(tc.memCurrent); // there is always one
	dlg_assert(tc.memCurrent->offset <= tc.memCurrent->size);
	size = align(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);

	// assertCanary(*tc.memCurrent);

	// fast path: enough memory available directly inside the command buffer,
	// simply align and advance the offset
	auto newBlockSize = size_t(tc.minBlockSize);
	dlg_assert(tc.memCurrent && tc.memRoot);
	dlg_assert(tc.memCurrent->offset % __STDCPP_DEFAULT_NEW_ALIGNMENT__ == 0u);
	if(tc.memCurrent->offset + size <= tc.memCurrent->size) {
		auto ret = data(*tc.memCurrent, tc.memCurrent->offset);
		tc.memCurrent->offset += size;
		return ret;
	}

	if(tc.memCurrent->next) {
		if(tc.memCurrent->next->size >= size) {
			tc.memCurrent = tc.memCurrent->next;
			tc.memCurrent->offset = size;
			return data(*tc.memCurrent, 0);
		}

		// TODO: Just insert it in between, no need to free blocks here I guess
		dlg_warn("Giant local allocation (size {}); have to free previous blocks", size);
		freeBlocks(tc.memCurrent->next);
		tc.memCurrent->next = nullptr;
	}

	// not enough memory available in last block, allocate new one
	newBlockSize = std::min<size_t>(tc.blockGrowFac * tc.memCurrent->size, tc.maxBlockSize);
	newBlockSize = std::max<size_t>(newBlockSize, size);

	dlg_assert(!tc.memCurrent->next);
	auto& newBlock = createMemBlock(newBlockSize, tc.memCurrent);
	tc.memCurrent->next = &newBlock;

	tc.memCurrent = &newBlock;
	tc.memCurrent->offset = size;
	return data(*tc.memCurrent, 0);
}

// Allocated memory from ThreadContext.
// Will simply release all allocated memory by reset the allocation offset in the
// current ThreadContext when this object is destroyed.
// The object must therefore not be used/moved across thread boundaries or
// alloc to different ThreadMemScope objects be incorrectly mixed (only allowed
// in a stack-like manner).
// When this is used, it must be the only mechanism by which memory
// from the thread context is allocated.
struct ThreadMemScope {
	ThreadMemBlock* block {};
	size_t offset {};

	// only for debugging, making sure that we never use multiple
	// ThreadMemScope objects at the same time in any way not resembling
	// a stack.
#ifdef VIL_DEBUG
	size_t sizeAllocated {};
	std::byte* current {};
#endif // VIL_DEBUG


	// TODO: make naming here consistent with command allocator.
	// alloc -> allocSpan0
	// allocUndef -> allocSpan

	template<typename T>
	span<T> alloc(size_t n) {
		auto ptr = allocRaw<T>(n);
		return {ptr, n};
	}

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
		static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);
		auto ptr = reinterpret_cast<T*>(this->allocRaw(sizeof(T) * n));
		new(ptr) T[n]();
		return ptr;
	}

	template<typename T>
	T* allocRawUndef(size_t n = 1) {
		static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);
		auto ptr = reinterpret_cast<T*>(this->allocRaw(sizeof(T) * n));
		new(ptr) T[n];
		return ptr;
	}

	inline std::byte* allocRaw(size_t size) {
		auto& tc = ThreadContext::get();

#ifdef VIL_DEBUG
		dlg_assertm(data(*tc.memCurrent, tc.memCurrent->offset) == this->current,
			"Invalid non-stacking interleaving of ThreadMemScope detected");
#endif // VIL_DEBUG

		auto* ptr = vil::allocate(tc, size);

#ifdef VIL_DEBUG
		current = ptr + align(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
		sizeAllocated += align(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
#endif // VIL_DEBUG

		return ptr;
	}

	ThreadMemScope(); // stores current state
	~ThreadMemScope(); // resets state
};

template<typename T>
class ThreadMemoryAllocator {
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
		dlg_assert(alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);
		return memScope_->allocRaw(bytes);
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
