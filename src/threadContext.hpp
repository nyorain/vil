#pragma once

#include <fwd.hpp>
#include <cstdlib>
#include <vector>
#include <cassert>
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
	// following: std::byte[size]
};

struct ThreadContext {
	static ThreadContext& get();

	// We grow block sizes exponentially, up to a maximum
	static constexpr auto minBlockSize = 16 * 1024;
	static constexpr auto maxBlockSize = 16 * 1024 * 1024;
	static constexpr auto blockGrowFac = 2;

	ThreadMemBlock* memRoot {};
	ThreadMemBlock* memCurrent {};
	size_t memOffset {};

	ThreadContext();
	~ThreadContext();
};

// Guaranteed to be aligned with __STDCPP_DEFAULT_NEW_ALIGNMENT__
std::byte* allocate(ThreadContext& tc, size_t size);
void free(ThreadContext& tc, const std::byte* ptr, size_t size);

// NOTE: object of this allocator must never cross thread boundaries.
// Must only be used for local stuff. Objects must also always be
// destroyed in the order they were created, in a stack-like fashion.
template<typename T>
struct ThreadContextAllocator {
	using is_always_equal = std::true_type;
	using value_type = T;

	T* allocate(size_t n) {
		auto ptr = vil::allocate(ThreadContext::get(), sizeof(T) * n);
		return reinterpret_cast<T*>(ptr);
	}

	void deallocate(const T* ptr, size_t n) const noexcept {
		free(ThreadContext::get(), reinterpret_cast<const std::byte*>(ptr), n * sizeof(T));
	}
};

template<typename T>
bool operator==(const ThreadContextAllocator<T>&, const ThreadContextAllocator<T>&) noexcept {
	return true;
}

template<typename T>
bool operator!=(const ThreadContextAllocator<T>&, const ThreadContextAllocator<T>&) noexcept {
	return false;
}

template<typename T>
struct LocalVector {
	using iterator = T*;
	using const_iterator = const T*;

	size_t size_;
	T* data_ {};

	LocalVector(const T* start, size_t size) : size_(size) {
		if(size) {
			data_ = ThreadContextAllocator<T>().allocate(size);
			new(data_) T[size_];
			std::copy(start, start + size, data_);
		}
	}

	LocalVector(size_t size = 0u) : size_(size) {
		if(size_) {
			data_ = ThreadContextAllocator<T>().allocate(size);
			new(data_) T[size_]();
		}
	}

	~LocalVector() {
		if(size_) {
			std::destroy(begin(), end());
			ThreadContextAllocator<T>().deallocate(data_, size_);
		}
	}

	LocalVector(const LocalVector&) = delete;
	LocalVector& operator=(const LocalVector&) = delete;

	/*
	LocalVector(LocalVector&& rhs) noexcept : size_(rhs.size_), data_(rhs.data_) {
		rhs.data_ = {};
		rhs.size_ = {};
	}

	LocalVector& operator=(LocalVector&& rhs) noexcept {
		// this is usually a sign for an error, there are strict requirements
		// for construction/destruction order of LocalVector, it should
		// not happen here
		dlg_assert(empty());
		size_ = rhs.size_;
		data_ = rhs.data_;
		rhs.data_ = {};
		rhs.size_ = {};
		return *this;
	}
	*/

	iterator begin() noexcept { return data_; }
	iterator end() noexcept { return data_ + size_; }
	const_iterator begin() const noexcept { return data_; }
	const_iterator end() const noexcept { return data_ + size_; }

	T* data() noexcept { return data_; }
	const T* data() const noexcept { return data_; }

	T& operator[](size_t i) { assert(i < size_); return data_[i]; }
	const T& operator[](size_t i) const { assert(i < size_); return data_[i]; }

	size_t size() const noexcept { return size_; }
	bool empty() const noexcept { return size() == 0; }
};

// Offset more flexibility compared to LocalVector, e.g. for in-loop
// allocation. All memory allocated by it from the ThreadContext will simply
// be released when this object is destroyed.
// When this is used, it must be the only mechanism by which memory
// from the thread context is allocated, i.e. it must not be mixed
// with manual allocation or LocalVector.
struct ThreadMemScope {
	ThreadMemBlock* block {};
	size_t offset {};
	size_t sizeAllocated {};

	template<typename T>
	span<T> alloc(size_t n) {
		auto ptr = ThreadContextAllocator<T>().allocate(n);
		new(ptr) T[n]();
		sizeAllocated += align(sizeof(T) * n, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
		return {ptr, n};
	}

	template<typename T>
	span<T> copy(T* data, size_t n) {
		auto ptr = ThreadContextAllocator<T>().allocate(n);
		new(ptr) T[n]();
		std::memcpy(ptr, data, n * sizeof(T));
		sizeAllocated += align(sizeof(T) * n, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
		return {ptr, n};
	}

	ThreadMemScope(); // stores current state
	~ThreadMemScope(); // resets state
};

} // namespace vil
