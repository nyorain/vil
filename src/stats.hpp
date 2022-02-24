#pragma once

#include <fwd.hpp>
#include <atomic>

namespace vil {

struct DebugStats {
	static DebugStats& get();

	std::atomic<u32> aliveRecords {};
	std::atomic<u32> aliveDescriptorCopies {};
	std::atomic<u32> aliveDescriptorSets {};
	std::atomic<u32> aliveBuffers {};
	std::atomic<u32> aliveImagesViews {};
	std::atomic<u32> aliveHookRecords {};
	std::atomic<u32> aliveHookStates {};

	std::atomic<u64> threadContextMem {};
	std::atomic<u64> commandMem {};
	std::atomic<u64> descriptorCopyMem {};
	std::atomic<u64> descriptorPoolMem {};

	std::atomic<u64> ownBufferMem {};
	std::atomic<u64> copiedImageMem {};
};

} // namespace vil

