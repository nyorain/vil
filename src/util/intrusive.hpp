#pragma once

#include <type_traits>
#include <utility>
#include <tuple>
#include <memory>
#include <util/dlg.hpp>

namespace vil {

constexpr struct AcquireOwnershipTag {} acquireOwnership;

template<typename T>
void incRefCount(T& obj) {
	obj.refCount.fetch_add(1u, std::memory_order_relaxed);
}

template<typename T, typename Deleter = std::default_delete<T>>
void decRefCount(T& obj) {
	dlg_assert(obj.refCount.load() > 0u);
	if(obj.refCount.fetch_sub(1u, std::memory_order_acq_rel) == 1u) {
		Deleter()(&obj);
	}
}

template<typename T, typename H>
class HandledPtr {
public:
	using pointer = T*;

public:
	HandledPtr() noexcept(std::is_nothrow_constructible_v<H>) :
			storage_{nullptr, H{}} {
		static_assert(std::is_nothrow_invocable_v<decltype(&H::dec), H, T&>);
		static_assert(std::is_nothrow_invocable_v<decltype(&H::inc), H, T&>);
	}

	void reset(T* ptr = nullptr) noexcept {
		// TODO: details (order of operation, might be relevant if ~ptr_
		// has access to this wrapper) don't match unique_ptr::reset.
		// They probably should.
		if(getPointer()) {
			getHandler().dec(*getPointer());
			getPointer() = nullptr;
		}

		getPointer() = ptr;
		if(ptr) {
			getHandler().inc(*ptr);
		}
	}

	~HandledPtr() noexcept {
		reset();
	}

	explicit HandledPtr(T* ptr, H h = {}) noexcept : storage_{ptr, std::move(h)} {
		if(getPointer()) {
			getHandler().inc(*getPointer());
		}
	}
	explicit HandledPtr(AcquireOwnershipTag, T* ptr, H h = {}) noexcept : storage_{ptr, std::move(h)} {
	}
	HandledPtr(std::nullptr_t, H h = {}) noexcept : storage_{nullptr, std::move(h)} {}

	HandledPtr(const HandledPtr& rhs) noexcept : storage_(rhs.storage_) {
		if(getPointer()) {
			getHandler().inc(*getPointer());
		}
	}

	HandledPtr& operator=(const HandledPtr& rhs) noexcept {
		reset();
		storage_ = rhs.storage_;
		if(getPointer()) {
			getHandler().inc(*getPointer());
		}
		return *this;
	}

	HandledPtr(HandledPtr&& rhs) noexcept : storage_(std::move(rhs.storage_)) {
		std::get<0>(rhs.storage_) = nullptr;
	}

	HandledPtr& operator=(HandledPtr&& rhs) noexcept {
		reset();
		storage_ = std::move(rhs.storage_);
		std::get<0>(rhs.storage_) = nullptr;
		return *this;
	}

	T* get() const noexcept { return std::get<0>(storage_); }
	H& getHandler() const noexcept { return getHandler(); }

	[[nodiscard]] T* release() noexcept {
		auto ret = get();
		getPointer() = nullptr;
		return ret;
	}

	operator bool() const noexcept { return get(); }
	T* operator->() const noexcept { return get(); }
	T& operator*() const noexcept { return *get(); }

protected:
	T*& getPointer() noexcept { return std::get<0>(storage_); }
	H& getHandler() noexcept { return std::get<1>(storage_); }

	std::tuple<T*, H> storage_; // empty-object-optimization for H
};

template<typename T, typename Deleter>
struct RefCountHandler {
	// NOTE: we assume that increasing/decreasing ref count is noexcept
	// See https://stackoverflow.com/questions/41424539 for memory order rationle
	void inc(T& obj) const noexcept { incRefCount(obj); }
	void dec(T& obj) const noexcept { decRefCount<T, Deleter>(obj); }
};

template<typename T>
struct FinishHandler {
	FinishHandler() = default;

	// Can't be copied; inc not possible
	FinishHandler(const FinishHandler&) = delete;
	FinishHandler& operator=(const FinishHandler&) = delete;

	FinishHandler(FinishHandler&&) noexcept = default;
	FinishHandler& operator=(FinishHandler&&) noexcept = default;

	void inc(T&) const noexcept {}
	void dec(T& obj) const noexcept {
		static_assert(std::is_nothrow_invocable_v<decltype(&T::finish), T>);
		obj.finish();
	}
};

template<typename T, typename H>
bool operator==(const HandledPtr<T, H>& a, const HandledPtr<T, H>& b) {
	return a.get() == b.get();
}

template<typename T, typename H>
bool operator==(const HandledPtr<T, H>& a, const T* b) {
	return a.get() == b;
}

template<typename T, typename H>
bool operator==(const T* b, const HandledPtr<T, H>& a) {
	return a.get() == b;
}

template<typename T, typename H>
bool operator!=(const HandledPtr<T, H>& a, const HandledPtr<T, H>& b) {
	return a.get() != b.get();
}

template<typename T, typename H>
bool operator!=(const HandledPtr<T, H>& a, const T* b) {
	return a.get() != b;
}

template<typename T, typename H>
bool operator!=(const T* b, const HandledPtr<T, H>& a) {
	return a.get() != b;
}

} // namspace vil

namespace std {
	template<typename T, typename H>
	struct hash<vil::HandledPtr<T, H>> {
		// See cppreference is_transparent
		// We need C++20 for associative transparent lookup
		using transparent_key_equal = std::equal_to<>;
		using hash_type = std::hash<const T*>;

		// TODO: remove this when all compilers implement P1690R3.
		// GCC 11.1 does not have it
		using is_transparent = struct UghWtf {};

		size_t operator()(const vil::HandledPtr<T, H>& x) const {
			return hash_type{}(x.get());
		}

		size_t operator()(const T* x) const {
			return hash_type{}(x);
		}
	};
}
