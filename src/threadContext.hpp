#pragma once

#include <fwd.hpp>
#include <cstdlib>
#include <vector>

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

	ThreadMemBlock* memRoot {};
	ThreadMemBlock* memCurrent {};
	size_t memOffset {};

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

	void deallocate(T* ptr, size_t n) const noexcept {
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

	LocalVector(size_t size = 0u) : size_(size) {
		if(size_) {
			data_ = ThreadContextAllocator<T>().allocate(size);
			new(data_) T[size_];
		}
	}

	~LocalVector() {
		if(size_) {
			delete[] data_;
			ThreadContextAllocator<T>().free(data_, size_);
		}
	}

	LocalVector(const LocalVector&) = delete;
	LocalVector& operator=(const LocalVector&) = delete;

	LocalVector(LocalVector&& rhs) noexcept : size_(rhs.size_), data_(rhs.data_) {
		rhs.data_ = {};
		rhs.size_ = {};
		return *this;
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

	iterator begin() noexcept { return data_; }
	iterator end() noexcept { return data_ + size_; }
	const_iterator begin() const noexcept { return data_; }
	const_iterator end() const noexcept { return data_ + size_; }

	T* data() noexcept { return data_; }
	const T* data() const noexcept { return data_; }

	T& operator[](size_t i) { dlg_assert(i < size_); return data_[i]; }
	const T& operator[](size_t i) const { dlg_assert(i < size_); return data_[i]; }

	size_t size() const { return size_; }
	bool empty() const { return size() > 0; }
};

// TODO: use something like this instead for memory scoping instead of
// strictly realying on allocate/free orders
// struct ThreadMemContext {
// 	ThreadMemBlock* block {};
// 	size_t offset {};
//
// 	ThreadMemContext(); // stores current state
// 	~ThreadMemContext(); // resets state
// };

} // namespace vil
