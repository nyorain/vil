#pragma once

#include <fwd.hpp>
#include <cstdlib>
#include <vector>
#include <cassert>
#include <cstring>
#include <memory_resource>
#include <util/util.hpp>
#include <util/profiling.hpp>
#include <util/linalloc.hpp>
#include <util/dlg.hpp>

// per-thread allocator for temporary local memory, allocated in stack-like
// fashion. Due to its strict requirement, only useful to create one-shot
// continuous sequences.

namespace vil {

// All data we need per-thread. Currently only used for stack-like
// dynamic memory allocation.
struct ThreadContext {
	// NOTE: C++ does not clearly specify when its constructor will
	// be called, just that it's before it's used the first time.
	// This means we might be creating them for *every* thread even
	// if its never used there.
	static thread_local ThreadContext instance;

	static std::mutex mutex_;
	static std::vector<ThreadContext*> contexts_;

	// Only to be used in a scoped manner, via ThreadMemScope.
	LinAllocator linalloc_;

	ThreadContext() {
		// TODO: hacky
		std::lock_guard lock(mutex_);
		contexts_.push_back(this);
	}

	~ThreadContext() {
		// assert that all memory was freed
		dlg_assert(linalloc_.empty());

		// TODO: hacky
		std::lock_guard lock(mutex_);
		auto it = find(contexts_, this);
		dlg_assert(it != contexts_.end());
		contexts_.erase(it);
	}
};

struct ThreadMemScope : LinAllocScope {
	inline ThreadMemScope() : LinAllocScope(ThreadContext::instance.linalloc_) {}
};

} // namespace vil
