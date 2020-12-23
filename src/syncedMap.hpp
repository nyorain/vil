#pragma once

#include <shared_mutex>
#include <unordered_map>
#include <memory>
#include <cassert>
#include <intrusive.hpp>

namespace fuen {

template<typename T>
struct SmartPtrFactory;

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
		return it == map.end() ? nullptr : it->second.get();
	}

	// Expects an element in the map, finds and returns it.
	// Unlike operator[], will never create the element.
	// Error to call this with a key that isn't present.
	T& get(const K& key) {
		std::shared_lock lock(*mutex);
		return getLocked(key);
	}

	T& getLocked(const K& key) {
		auto it = map.find(key);
		assert(it != map.end());
		return *it->second.get();
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
	std::pair<T*, bool> emplace(Args&&... args) {
		std::lock_guard lock(*mutex);
		auto [it, success] = map.emplace(std::forward<Args>(args)...);
		return {it->second.get(), success};
	}

	template<class... Args>
	T& mustEmplace(Args&&... args) {
		auto [ptr, success] = this->emplace(std::forward<Args>(args)...);
		assert(success);
		return *ptr;
	}

	// Asserts that element is really new
	template<typename V = T, class... Args>
	T& add(const K& key, Args&&... args) {
		auto elem = SmartPtrFactory<P<V>>::create(std::forward<Args>(args)...);
		return this->mustEmplace(key, std::move(elem));
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

	// Pretty much only provided for shared ptr specialization.
	template<typename = void>
	std::weak_ptr<T> getWeakPtrLocked(const K& key) {
		auto it = map.find(key);
		assert(it != map.end());
		return std::weak_ptr(it->second);
	}

	template<typename = void>
	std::weak_ptr<T> getWeakPtr(const K& key) {
		std::shared_lock lock(*mutex);
		return getWeakPtrLocked(key);
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
	std::shared_mutex* mutex;
	UnorderedMap map;
};

template<typename T>
struct SmartPtrFactory<std::unique_ptr<T>> {
	template<typename... Args>
	static std::unique_ptr<T> create(Args&&... args) {
		return std::make_unique<T>(std::forward<Args>(args)...);
	}
};

template<typename T>
struct SmartPtrFactory<std::shared_ptr<T>> {
	template<typename... Args>
	static std::shared_ptr<T> create(Args&&... args) {
		return std::make_shared<T>(std::forward<Args>(args)...);
	}
};

template<typename T>
struct SmartPtrFactory<IntrusivePtr<T>> {
	template<typename... Args>
	static IntrusivePtr<T> create(Args&&... args) {
		return IntrusivePtr<T>(new T(std::forward<Args>(args)...));
	}
};

template<typename K, typename T>
using SyncedUniqueUnorderedMap = SyncedUnorderedMap<K, T, std::unique_ptr>;

template<typename K, typename T>
using SyncedSharedUnorderedMap = SyncedUnorderedMap<K, T, std::shared_ptr>;

template<typename K, typename T>
using SyncedIntrusiveUnorderedMap = SyncedUnorderedMap<K, T, IntrusivePtr>;

} // namespace fuen
