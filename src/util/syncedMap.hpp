#pragma once

#include <shared_mutex>
#include <unordered_map>
#include <memory>
#include <cassert>
#include <util/intrusive.hpp>
#include <util/debugMutex.hpp>
#include <util/profiling.hpp>

namespace vil {

template<typename T>
struct HandlePtrFactory;

template<typename T>
struct WrappedHandle {
	void* dispatch {};
	std::aligned_storage_t<sizeof(T), alignof(T)> obj_;

	template<typename... Args>
	WrappedHandle(Args&&... args) {
		// This is important so that the loader can correctly access the
		// dispatch table. That's why we use std::aligned_storage_t instead
		// of directly holding the object.
		static_assert(std::is_standard_layout_v<WrappedHandle<T>>);
		new(&obj_) T(std::forward<Args>(args)...);
	}

	~WrappedHandle() {
		obj().~T();
	}

	T& obj() { return *std::launder(reinterpret_cast<T*>(&obj_)); }
};

template<typename T, typename Deleter = std::default_delete<WrappedHandle<T>>>
struct WrappedRefCount {
	void inc(WrappedHandle<T>& wrapped) const noexcept { ++wrapped.obj().refCount; }
	void dec(WrappedHandle<T>& wrapped) const noexcept {
		if(--wrapped.obj().refCount == 0) {
			Deleter()(&wrapped);
		}
	}
};

template<typename T>
struct IntrusiveWrappedPtr : HandledPtr<WrappedHandle<T>, WrappedRefCount<T>> {
	using Base = HandledPtr<WrappedHandle<T>, WrappedRefCount<T>>;
	using Base::Base;

	T* get() const noexcept {
		auto ptr = Base::get();
		return ptr ? &ptr->obj() : nullptr;
	}

	T* operator->() const noexcept {
		return &Base::get()->obj();
	}

	T& operator*() const noexcept {
		return Base::get()->obj();
	}

	WrappedHandle<T>* wrapped() const noexcept {
		return Base::get();
	}
};

template<typename T>
struct UniqueWrappedPtr : std::unique_ptr<WrappedHandle<T>> {
	using Base = std::unique_ptr<WrappedHandle<T>>;
	using Base::Base;

	T* get() const noexcept {
		auto ptr = Base::get();
		return ptr ? &ptr->obj() : nullptr;
	}

	T* operator->() const noexcept {
		return get();
	}

	T& operator*() const noexcept {
		return Base::get()->obj();
	}

	WrappedHandle<T>* wrapped() const noexcept {
		return Base::get();
	}
};

// Synchronized unordered map.
// Elements are stored in P<T>'s (where P should be a smart pointer type such
// as unique_ptr or shared_ptr) making sure that as long as two
// threads never operate on the same entry and lookup/creation/destruction
// of entries is synchronized, everything just works.
// You should never (outside of a lock and we try to limit the locks to
// the immediate lookup/creation/destruction sections) work with iterators
// or P<T> refs of this map since they might get destroyed
// at *any* moment, when the unordered map needs a rehash. But the underlying
// elements are guaranteed to survive.
// The mutex will always be unlocked when the destructor of an object is run.
template<typename K, typename T, template<typename...> typename P>
class SyncedUnorderedMap {
public:
	using UnorderedMap = std::unordered_map<K, P<T>>;

	P<T> moveLocked(const K& key) {
		assertOwned(*mutex);

		auto it = map.find(key);
		if(it == map.end()) {
			return nullptr;
		}

		auto ret = std::move(it->second);
		map.erase(it);
		return ret;
	}

	P<T> move(const K& key) {
		std::lock_guard lock(*mutex);
		return moveLocked(key);
	}

	P<T> mustMove(const K& key) {
		auto ret = move(key);
		assert(ret);
		return ret;
	}

	P<T> mustMoveLocked(const K& key) {
		auto ret = moveLocked(key);
		assert(ret);
		return ret;
	}

	// Mskes sure to run destructor outside of lock
	std::size_t erase(const K& key) {
		// std::lock_guard lock(*mutex);
		// return map.erase(key);
		auto ptr = move(key);
		return ptr ? 1u : 0u;
	}

	void mustErase(const K& key) {
		// std::lock_guard lock(*mutex);
		// auto count = map.erase(key);
		// assert(count);
		// return count;
		auto ptr = mustMove(key);
		(void) ptr;
	}

	T* find(const K& key) {
		std::shared_lock lock(*mutex);
		auto it = map.find(key);
		return it == map.end() ? nullptr : &*it->second;
	}

	// Expects an element in the map, finds and returns it.
	// Unlike operator[], will never create the element.
	// Error to call this with a key that isn't present.
	T& get(const K& key) {
		std::shared_lock lock(*mutex);
		return getLocked(key);
	}

	T& getLocked(const K& key) {
		assertOwnedOrShared(*mutex);

		auto it = map.find(key);
		assert(it != map.end());
		return *it->second;
	}

	template<class O>
	bool contains(const O& x) const {
		// TODO C++20, use map.contains
		std::shared_lock lock(*mutex);
		return map.find(x) != map.end();
	}

	T& operator[](const K& key) {
		std::lock_guard lock(*mutex);
		return map[key];
	}

	// emplace methods may be counter-intuitive.
	// You must actually pass a P<T> as value.
	// Might wanna use add() instead.
	template<class... Args>
	std::pair<P<T>*, bool> emplace(Args&&... args) {
		std::lock_guard lock(*mutex);
		auto [it, success] = map.emplace(std::forward<Args>(args)...);
		return {&it->second, success};
	}

	template<class... Args>
	P<T>& mustEmplace(Args&&... args) {
		auto [ptr, success] = this->emplace(std::forward<Args>(args)...);
		assert(success);
		return *ptr;
	}

	// Asserts that element is really new
	template<typename V = T, class... Args>
	P<T>& addPtr(const K& key, Args&&... args) {
		auto elem = HandlePtrFactory<P<V>>::create(std::forward<Args>(args)...);
		return this->mustEmplace(key, std::move(elem));
	}

	template<typename V = T, class... Args>
	T& add(const K& key, Args&&... args) {
		auto elem = HandlePtrFactory<P<V>>::create(std::forward<Args>(args)...);
		return *this->mustEmplace(key, std::move(elem));
	}

	// Keep in mind they can immediately be out-of-date.
	bool empty() const {
		std::shared_lock lock(*mutex);
		return map.empty();
	}

	std::size_t size() const {
		std::shared_lock lock(*mutex);
		return map.size();
	}

	// Only allowed to call this function when P<T> is copyable.
	// Useful for shared pointers.
	template<typename = void>
	P<T> getPtr(const K& key) {
		static_assert(std::is_copy_constructible_v<P<T>>);
		std::shared_lock lock(*mutex);
		auto it = map.find(key);
		assert(it != map.end());
		return it->second;
	}

	template<typename = void>
	P<T> findPtr(const K& key) {
		static_assert(std::is_copy_constructible_v<P<T>>);
		std::shared_lock lock(*mutex);
		auto it = map.find(key);
		if(it == map.end()) {
			return {};
		}

		return it->second;
	}

	// Can also be used directly, but take care!
	// SharedMutex* mutex;
	SharedLockableBase(DebugSharedMutex)* mutex;
	UnorderedMap map;
};

template<typename T>
struct HandlePtrFactory<std::unique_ptr<T>> {
	template<typename... Args>
	static std::unique_ptr<T> create(Args&&... args) {
		return std::make_unique<T>(std::forward<Args>(args)...);
	}
};

template<typename T>
struct HandlePtrFactory<UniqueWrappedPtr<T>> {
	template<typename... Args>
	static UniqueWrappedPtr<T> create(Args&&... args) {
		return UniqueWrappedPtr<T>(new WrappedHandle<T>(std::forward<Args>(args)...));
	}
};

template<typename T>
struct HandlePtrFactory<IntrusivePtr<T>> {
	template<typename... Args>
	static IntrusivePtr<T> create(Args&&... args) {
		return IntrusivePtr<T>(new T(std::forward<Args>(args)...));
	}
};

template<typename T>
struct HandlePtrFactory<IntrusiveWrappedPtr<T>> {
	template<typename... Args>
	static IntrusiveWrappedPtr<T> create(Args&&... args) {
		return IntrusiveWrappedPtr<T>(new WrappedHandle<T>(std::forward<Args>(args)...));
	}
};

template<typename T> using IdentityT = T;
template<typename T> using PointerT = T*;

template<typename K, typename T>
using SyncedRawUnorderedMap = SyncedUnorderedMap<K, T, PointerT>;

template<typename K, typename T>
using SyncedUniqueUnorderedMap = SyncedUnorderedMap<K, T, std::unique_ptr>;

template<typename K, typename T>
using SyncedUniqueWrappedUnorderedMap = SyncedUnorderedMap<K, T, UniqueWrappedPtr>;

template<typename K, typename T>
using SyncedIntrusiveUnorderedMap = SyncedUnorderedMap<K, T, IntrusivePtr>;

template<typename K, typename T>
using SyncedIntrusiveWrappedUnorderedMap = SyncedUnorderedMap<K, T, IntrusiveWrappedPtr>;

} // namespace vil
