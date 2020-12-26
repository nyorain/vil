#pragma once

#include <fwd.hpp>
#include <type_traits>

namespace fuen {

template<typename T>
class IntrusivePtr {
public:
	IntrusivePtr() noexcept = default;

	void reset() noexcept(std::is_nothrow_destructible_v<T>) {
		// NOTE: fetch_sub returns the value before decrementing,
		// so we delete the ptr when it was 1.
		// TODO: details (order of operation, might be relevant if ~ptr_
		// has access to this wrapper) don't match unique_ptr::reset.
		// They probably should.
		if(ptr_ && ptr_->refCount.fetch_sub(1) == 1) {
			delete ptr_;
		}

		ptr_ = nullptr;
	}

	~IntrusivePtr() noexcept(noexcept(reset())) { reset(); }

	explicit IntrusivePtr(T* ptr) noexcept : ptr_(ptr) {
		if(ptr_) {
			ptr_->refCount.fetch_add(1);
		}
	}
	IntrusivePtr(std::nullptr_t) noexcept : ptr_{} {}

	IntrusivePtr(const IntrusivePtr& rhs) noexcept : ptr_(rhs.ptr_) {
		if(ptr_) {
			ptr_->refCount.fetch_add(1);
		}
	}
	IntrusivePtr& operator=(const IntrusivePtr& rhs) noexcept(noexcept(reset())) {
		reset();
		ptr_ = rhs.ptr_;
		if(ptr_) {
			ptr_->refCount.fetch_add(1);
		}
		return *this;
	}

	IntrusivePtr(IntrusivePtr&& rhs) noexcept : ptr_(rhs.ptr_) {
		rhs.ptr_ = nullptr;
	}
	IntrusivePtr& operator=(IntrusivePtr&& rhs) noexcept(noexcept(reset())) {
		reset();
		ptr_ = rhs.ptr_;
		rhs.ptr_ = nullptr;
		return *this;
	}

	operator bool() const noexcept { return ptr_; }
	T* operator->() const noexcept { return ptr_; }
	T& operator*() const noexcept { return *ptr_; }
	T* get() const noexcept { return ptr_; }

protected:
	T* ptr_ {};
};

} // namspace fuen
