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

	template<typename T>
	span<T> alloc(size_t n) {
		auto ptr = allocRaw<T>(n);
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

	std::byte* allocRaw(size_t size);

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
