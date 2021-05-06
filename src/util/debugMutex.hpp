#pragma once

#include <unordered_set>
#include <shared_mutex>
#include <thread>
#include <atomic>
#include <dlg/dlg.hpp>
#include <tracy/Tracy.hpp>

namespace vil {

// TODO: use default mutex classes in some release build.
// We only ever use `mutex.owned()` inside dlg_assert expressions, must
// match the switch here to that. Or maybe also introduce assert_owned,
// assert_not_owned macros or something
#if 0

using SharedMutex = std::shared_mutex;
using Mutex = std::mutex;

#define vil_assert_owned(x)
#define vil_assert_not_owned(x)

#else // NDEBUG

// std::shared_mutex that knows whether it's locked.
// Using this information in actual code logic is a terrible idea but
// it's useful to find issues (e.g. a mutex isn't locked when we expected
// it to be) with the unfortunately at times complicated threading
// assumptions/guarantees for vil functions.
struct SharedMutex {
	std::shared_mutex mtx_;
	std::atomic<std::thread::id> owner_ {};
	std::unordered_set<std::thread::id> shared_ {};
	mutable std::mutex sharedMutex_ {};

	void lock() {
		dlg_assert(!owned());
		dlg_assert(!ownedShared());
		mtx_.lock();
		dlg_assert(owner_ == std::thread::id{});
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
			dlg_assert(owner_ == std::thread::id{});
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

struct Mutex {
	std::mutex mtx_;
	std::atomic<std::thread::id> owner_ {};

	void lock() {
		dlg_assert(!owned());
		mtx_.lock();
		dlg_assert(owner_ == std::thread::id{});
		owner_.store(std::this_thread::get_id());
	}

	void unlock() {
		dlg_assert(owned());
		owner_.store({});
		mtx_.unlock();
	}

	bool try_lock() {
		dlg_assert(!owned());
		auto ret = mtx_.try_lock();
		if(ret) {
			dlg_assert(owner_ == std::thread::id{});
			owner_ = std::this_thread::get_id();
		}
		return ret;
	}

	bool owned() const {
		return owner_.load() == std::this_thread::get_id();
	}
};

bool owned(const Mutex& m) { return m.owned(); }
bool owned(const SharedMutex& m) { return m.owned(); }
bool ownedShared(const SharedMutex& m) { return m.ownedShared(); }

bool owned(const tracy::Lockable<Mutex>& m) { return m.inner().owned(); }
bool owned(const tracy::SharedLockable<SharedMutex>& m) { return m.inner().owned(); }
bool ownedShared(const tracy::SharedLockable<SharedMutex>& m) { return m.inner().ownedShared(); }

#define vil_assert_owned(x) dlg_assert(owned(x))
#define vil_assert_not_owned(x) dlg_assert(!owned(x))

#endif // NDEBUG

} // namespace vil
