#pragma once

#include <unordered_set>
#include <shared_mutex>
#include <thread>
#include <atomic>
#include <util/dlg.hpp>
#include <util/profiling.hpp>

// TODO
#define tsa_guarded_by(x) __attribute__((guarded_by(x)))
#define tsa_pt_guarded_by(x) __attribute__((pt_guarded_by(x)))
#define tsa_requires(x) __attribute__((requires_capability(x)))
#define tsa_lock_returned(x) __attribute__((lock_returned(x)))
#define tsa_acquire(x) __attribute__((acquire_capability(x)))
#define tsa_release(x) __attribute__((release_capability(x)))
#define tsa_acquire_shared(x) __attribute__((acquire_shared_capability(x)))
#define tsa_release_shared(x) __attribute__((release_shared_capability(x)))
#define tsa_capability(x) __attribute__((capability(x)))

namespace vil {

#ifndef VIL_DEBUG_MUTEX

using DebugSharedMutex = std::shared_mutex;
using DebugMutex = std::mutex;

#else // VIL_DEBUG_MUTEX

// std::shared_mutex that knows whether it's locked.
// Using this information in actual code logic is a terrible idea but
// it's useful to find issues (e.g. a mutex isn't locked when we expected
// it to be) with the unfortunately at times complicated threading
// assumptions/guarantees for vil functions.
struct tsa_capability("shared_mutex") DebugSharedMutex {
	std::shared_mutex mtx_;
	std::atomic<std::thread::id> owner_ {};
	std::unordered_set<std::thread::id> shared_ {};
	mutable std::mutex sharedMutex_ {};

	void lock() tsa_acquire() {
		dlg_assert(!owned());
		dlg_assert(!ownedShared());
		mtx_.lock();
		dlg_assert(owner_ == std::thread::id{});
		owner_.store(std::this_thread::get_id());
	}

	void unlock() tsa_release() {
		dlg_assert(owned());

		{
			std::lock_guard lock(sharedMutex_);
			dlg_assert(shared_.empty());
		}

		owner_.store({});
		mtx_.unlock();
	}

	// bool try_lock() {
	// 	dlg_assert(!owned());
	// 	dlg_assert(!ownedShared());
	// 	auto ret = mtx_.try_lock();
	// 	if(ret) {
	// 		dlg_assert(shared_.empty());
	// 		dlg_assert(owner_ == std::thread::id{});
	// 		owner_ = std::this_thread::get_id();
	// 	}
	// 	return ret;
	// }

	void lock_shared() tsa_acquire_shared() {
		dlg_assert(!owned());
		dlg_assert(!ownedShared());
		mtx_.lock_shared();
		dlg_assert(owner_.load() == std::thread::id{});

		std::lock_guard lock(sharedMutex_);
		shared_.insert(std::this_thread::get_id());
	}

	void unlock_shared() tsa_release_shared() {
		dlg_assert(ownedShared());
		dlg_assert(owner_ == std::thread::id{});

		{
			std::lock_guard lock(sharedMutex_);
			shared_.erase(std::this_thread::get_id());
		}

		mtx_.unlock_shared();
	}

	// bool try_lock_shared() {
	// 	dlg_assert(!owned());
	// 	dlg_assert(!ownedShared());
	// 	auto ret = mtx_.try_lock_shared();
	// 	if(ret) {
	// 		std::lock_guard lock(sharedMutex_);
	// 		dlg_assert(owner_.load() == std::thread::id{});
	// 		shared_.insert(std::this_thread::get_id());
	// 	}
	// 	return ret;
	// }

	bool owned() const {
		return owner_.load() == std::this_thread::get_id();
	}

	bool ownedShared() const {
		std::lock_guard lock(sharedMutex_);
		return shared_.find(std::this_thread::get_id()) != shared_.end();
	}
};

struct tsa_capability("mutex") DebugMutex {
	std::mutex mtx_;
	std::atomic<std::thread::id> owner_ {};

	void lock() tsa_acquire() {
		dlg_assert(!owned());
		mtx_.lock();
		dlg_assert(owner_ == std::thread::id{});
		owner_.store(std::this_thread::get_id());
	}

	void unlock() tsa_release() {
		dlg_assert(owned());
		owner_.store({});
		mtx_.unlock();
	}

	// bool try_lock() {
	// 	dlg_assert(!owned());
	// 	auto ret = mtx_.try_lock();
	// 	if(ret) {
	// 		dlg_assert(owner_ == std::thread::id{});
	// 		owner_ = std::this_thread::get_id();
	// 	}
	// 	return ret;
	// }

	bool owned() const {
		return owner_.load() == std::this_thread::get_id();
	}
};

inline bool owned(const DebugMutex& m) { return m.owned(); }
inline bool owned(const DebugSharedMutex& m) { return m.owned(); }
inline bool ownedShared(const DebugSharedMutex& m) { return m.ownedShared(); }

#ifdef TRACY_ENABLE
inline bool owned(const tracy::Lockable<DebugMutex>& m) { return m.inner().owned(); }
inline bool owned(const tracy::SharedLockable<DebugSharedMutex>& m) { return m.inner().owned(); }
inline bool ownedShared(const tracy::SharedLockable<DebugSharedMutex>& m) { return m.inner().ownedShared(); }
#endif // TRACY_ENABLE

#endif // VIL_DEBUG_MUTEX

// Tracy lockables.
// We might not want to use them in certain situations since we can have *a lot* of locks.
// But for small testcases and applications it's a useful optimization tool.
#if defined(TRACY_ENABLE) && defined(VIL_TRACY_MUTEX)
	#define vilDefSharedMutex(name) TracySharedLockable(DebugSharedMutex, name)
	#define vilDefMutex(name) TracyLockable(DebugMutex, name)
	using Mutex = LockableBase(DebugMutex);
	using SharedMutex = SharedLockableBase(DebugSharedMutex);
#else
	#define vilDefSharedMutex(name) DebugSharedMutex name
	#define vilDefMutex(name) DebugMutex name
	using Mutex = DebugMutex;
	using SharedMutex = DebugSharedMutex;
#endif

#ifdef VIL_DEBUG_MUTEX
	#define assertOwned(m) dlg_assert(owned(m))
	#define assertOwnedOrShared(m) dlg_assert(owned(m) || ownedShared(m))
	#define assertNotOwned(m) dlg_assert(!owned(m))
	#define assertNotOwnedOrShared(m) dlg_assert(!owned(m) && !ownedShared(m))
#else // VIL_DEBUG_MUTEX
	#define assertOwned(m) (void)0
	#define assertOwnedOrShared(m) (void)0
	#define assertNotOwned(m) (void)0
	#define assertNotOwnedOrShared(m) (void)0
#endif // VIL_DEBUG_MUTEX

} // namespace vil
