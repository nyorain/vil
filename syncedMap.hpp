#pragma once

#include <shared_mutex>
#include <unordered_map>
#include <memory>
#include <cassert>

namespace fuen {

// Synchronized unordered map.
// Elements are stored in unique_ptr's, making sure that as long as two
// threads never operate on the same entry and lookup/creation/destruction
// of entries is synchronized, everything just works.
// You should never (outside of a lock and we try to limit the locks to
// the immediate lookup/creation/destruction sections) work with iterators
// or unique_ptr<Value> refs of this map since they might get destroyed
// at *any* moment, when the unordered map needs a rehash.
template<typename K, typename T>
class SyncedUniqueUnorderedMap {
public:
	using UnorderedMap = std::unordered_map<K, std::unique_ptr<T>>;

	std::unique_ptr<T> move(const K& key) {
		std::lock_guard lock(*mutex);
		auto it = map.find(key);
		if(it == map.end()) {
			return nullptr;
		}

		auto ret = std::move(it->second);
		map.erase(it);
		return ret;
	}

	std::unique_ptr<T> mustMove(const K& key) {
		auto ret = move(key);
		assert(ret);
		return ret;
	}

	std::size_t erase(const K& key) {
		std::lock_guard lock(*mutex);
		return map.erase(key);
	}

	std::size_t mustErase(const K& key) {
		std::lock_guard lock(*mutex);
		auto count = map.erase(key);
		assert(count);
		return count;
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
	// You must actually pass a unique_ptr<T> as value.
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
		auto elem = std::make_unique<V>(std::forward<Args>(args)...);
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

	// Can also be used directly, but take care!
	std::shared_mutex* mutex;
	UnorderedMap map;
};

} // namespace fuen
