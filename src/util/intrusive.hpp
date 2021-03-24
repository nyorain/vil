#pragma once

#include <type_traits>
#include <utility>
#include <tuple>

namespace vil {

template<typename T, typename H>
class HandledPtr {
public:
	static constexpr auto noexceptDec = true; // std::is_nothrow_invocable_v<decltype(&H::dec), H, T*>;
	static constexpr auto noexceptInc = true; // std::is_nothrow_invocable_v<decltype(&H::inc), H, T*>;

	HandledPtr() noexcept(std::is_nothrow_constructible_v<H>) :
			storage_{nullptr, H{}} {
		static_assert(std::is_nothrow_invocable_v<decltype(&H::dec), H, T*>);
		static_assert(std::is_nothrow_invocable_v<decltype(&H::inc), H, T*>);
	}

	void reset(T* ptr = nullptr) noexcept(noexceptDec && noexceptInc) {
		// TODO: details (order of operation, might be relevant if ~ptr_
		// has access to this wrapper) don't match unique_ptr::reset.
		// They probably should.
		if(getPointer()) {
			getHandler().dec(getPointer());
			getPointer() = nullptr;
		}

		getPointer() = ptr;
		if(ptr) {
			getHandler().inc(ptr);
		}
	}

	~HandledPtr() noexcept(noexceptDec && std::is_nothrow_destructible_v<H>) {
		reset();
	}

	explicit HandledPtr(T* ptr, H h = {})
		noexcept(noexceptInc && std::is_nothrow_move_constructible_v<H>)
			: storage_{ptr, std::move(h)} {

		if(getPointer()) {
			getHandler().inc(getPointer());
		}
	}
	HandledPtr(std::nullptr_t, H h = {})
		noexcept(std::is_nothrow_move_constructible_v<H>)
			: storage_{nullptr, std::move(h)} {}

	HandledPtr(const HandledPtr& rhs)
		noexcept(std::is_nothrow_copy_assignable_v<H> && noexceptInc)
			: storage_{nullptr, H{}} {
		storage_ = rhs.storage_;
		if(getPointer()) {
			getHandler().inc(getPointer());
		}
	}

	HandledPtr& operator=(const HandledPtr& rhs)
			noexcept(std::is_nothrow_copy_assignable_v<H> && noexceptInc && noexceptDec) {
		reset();
		storage_ = rhs.storage_;
		if(getPointer()) {
			getHandler().inc(getPointer());
		}
		return *this;
	}

	HandledPtr(HandledPtr&& rhs) noexcept(std::is_nothrow_move_constructible_v<H>)
			: storage_(std::move(rhs.storage_)) {
		std::get<0>(rhs.storage_) = nullptr;
	}

	HandledPtr& operator=(HandledPtr&& rhs)
			noexcept(std::is_nothrow_move_assignable_v<H> && noexceptDec) {
		reset();
		storage_ = std::move(rhs.storage_);
		std::get<0>(rhs.storage_) = nullptr;
		return *this;
	}

	T* get() const noexcept { return std::get<0>(storage_); }
	H& getHandler() const noexcept { return getHandler(); }

	operator bool() const noexcept { return get(); }
	T* operator->() const noexcept { return get(); }
	T& operator*() const noexcept { return *get(); }

protected:
	T*& getPointer() noexcept { return std::get<0>(storage_); }
	H& getHandler() noexcept { return std::get<1>(storage_); }

	std::tuple<T*, H> storage_; // empty-object-optimization for H
};

template<typename T>
struct RefCountHandler {
	// NOTE: we assume that increasing/decreasing ref count is noexcept
	void inc(T* ptr) const noexcept { ++ptr->refCount; }
	// void dec(T* ptr) const noexcept(std::is_nothrow_destructible_v<T>) {
	void dec(T* ptr) const noexcept {
		static_assert(std::is_nothrow_destructible_v<T>);
		if(--ptr->refCount == 0) {
			delete ptr;
		}
	}
};

template<typename T>
struct FinishHandler {
	FinishHandler(const FinishHandler&) = delete;
	FinishHandler& operator=(const FinishHandler&) = delete;

	FinishHandler(FinishHandler&&) = default;
	FinishHandler& operator=(FinishHandler&&) = default;

	void inc(T*) const noexcept {}
	// void dec(T* ptr) const noexcept(noexcept(ptr->finish())) {
	void dec(T* ptr) const noexcept {
		static_assert(std::is_nothrow_invocable_v<decltype(&T::finish), T>);
		ptr->finish();
	}
};

template<typename T> using IntrusivePtr = HandledPtr<T, RefCountHandler<T>>;
template<typename T> using FinishPtr = HandledPtr<T, FinishHandler<T>>;

template<typename T, typename H>
bool operator==(const HandledPtr<T, H>& a, const HandledPtr<T, H>& b) {
	return a.get() == b.get();
}

template<typename T, typename H>
bool operator!=(const HandledPtr<T, H>& a, const HandledPtr<T, H>& b) {
	return a.get() != b.get();
}

} // namspace vil
