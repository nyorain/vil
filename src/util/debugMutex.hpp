#pragma once

#include <unordered_set>
#include <shared_mutex>
#include <thread>
#include <atomic>
#include <dlg/dlg.hpp>
#include <tracy/Tracy.hpp>

namespace vil {

// std::shared_mutex that knows whether it's locked.
// Using this information in actual code logic is a terrible idea but
// it's useful to find issues (e.g. a mutex isn't locked when we expected
// it to be) with the unfortunately at times complicated threading
// assumptions/guarantees for vil functions.
// TODO: only execute debugging instructions in debug mode. Adding the
// extra mutex is expensive.
struct DebugSharedMutex {
	TracySharedLockable(std::shared_mutex, mtx_) // like std::shared_mutex mtx_;
	std::atomic<std::thread::id> owner_ {};
	std::unordered_set<std::thread::id> shared_ {};
	mutable std::mutex sharedMutex_ {};

	void lock() {
		dlg_assert(!owned());
		dlg_assert(!ownedShared());
		mtx_.lock();
		owner_.store(std::this_thread::get_id());
	}

	void unlock() {
		dlg_assert(owned());

		{
			std::lock_guard lock(sharedMutex_);
			dlg_assert(shared_.empty());
		}

		owner_.store({});
		mtx_.unlock();
	}

	bool try_lock() {
		dlg_assert(!owned());
		dlg_assert(!ownedShared());
		auto ret = mtx_.try_lock();
		if(ret) {
			dlg_assert(shared_.empty());
			owner_ = std::this_thread::get_id();
		}
		return ret;
	}

	void lock_shared() {
		dlg_assert(!owned());
		dlg_assert(!ownedShared());
		mtx_.lock_shared();
		dlg_assert(owner_.load() == std::thread::id{});

		std::lock_guard lock(sharedMutex_);
		shared_.insert(std::this_thread::get_id());
	}

	void unlock_shared() {
		dlg_assert(ownedShared());
		dlg_assert(owner_ == std::thread::id{});

		{
			std::lock_guard lock(sharedMutex_);
			shared_.erase(std::this_thread::get_id());
		}

		mtx_.unlock_shared();
	}

	bool try_lock_shared() {
		dlg_assert(!owned());
		dlg_assert(!ownedShared());
		auto ret = mtx_.try_lock_shared();
		if(ret) {
			std::lock_guard lock(sharedMutex_);
			dlg_assert(owner_.load() == std::thread::id{});
			shared_.insert(std::this_thread::get_id());
		}
		return ret;
	}

	bool owned() const {
		return owner_.load() == std::this_thread::get_id();
	}

	bool ownedShared() const {
		std::lock_guard lock(sharedMutex_);
		return shared_.find(std::this_thread::get_id()) != shared_.end();
	}
};

} // namespace vil
