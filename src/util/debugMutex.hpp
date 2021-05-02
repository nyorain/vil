#pragma once

#include <unordered_set>
#include <shared_mutex>
#include <thread>
#include <dlg/dlg.hpp>

namespace vil {

// std::shared_mutex that knows whether it's locked.
// Using this information in actual code logic is a terrible idea but
// it's useful to find issues (e.g. a mutex isn't locked when we expected
// it to be) with the unfortunately at times complicated threading
// assumptions/guarantees for vil functions.
struct DebugSharedMutex : public std::shared_mutex {
	std::thread::id owner_ {};
	std::unordered_set<std::thread::id> shared_ {};

	void lock() {
		dlg_assert(!owned());
		dlg_assert(!ownedShared());
		std::shared_mutex::lock();
		owner_ = std::this_thread::get_id();
	}

	void unlock() {
		dlg_assert(owned());
		dlg_assert(shared_.empty());
		std::shared_mutex::unlock();
		owner_ = {};
	}

	bool try_lock() {
		dlg_assert(!owned());
		dlg_assert(!ownedShared());
		auto ret = std::shared_mutex::try_lock();
		if(ret) {
			dlg_assert(shared_.empty());
			owner_ = std::this_thread::get_id();
		}
		return ret;
	}

	void lock_shared() {
		dlg_assert(!owned());
		dlg_assert(!ownedShared());
		std::shared_mutex::lock_shared();
		dlg_assert(owner_ == std::thread::id{});
		shared_.insert(std::this_thread::get_id());
	}

	void unlock_shared() {
		dlg_assert(ownedShared());
		dlg_assert(owner_ == std::thread::id{});
		std::shared_mutex::lock_shared();
		shared_.erase(std::this_thread::get_id());
	}

	bool try_lock_shared() {
		dlg_assert(!owned());
		dlg_assert(!ownedShared());
		auto ret = std::shared_mutex::try_lock_shared();
		if(ret) {
			dlg_assert(owner_ == std::thread::id{});
			shared_.insert(std::this_thread::get_id());
		}
		return ret;
	}

	bool owned() const {
		return owner_ == std::this_thread::get_id();
	}

	bool ownedShared() const {
		return shared_.find(std::this_thread::get_id()) != shared_.end();
	}
};

} // namespace vil
