#pragma once

#include <fwd.hpp>
#include <vector>
#include <memory>
#include <deque>
#include <vk/vulkan.h>
#include <util/intrusive.hpp>

// Builder operations for a queue submission.
// We have to hook/track/manage a lot of stuff when an applications
// do a queue submission and our vkQueueSubmit implementation became
// too compliated so we split it up into the smaller components here.

namespace vil {

// PERF: we make a lot of allocations here and this is something
// that can be called multiple times per frame. Should likely use
// an allocator
struct QueueSubmitter {
	// Custom information for the submissions we build.
	// We might have to modify the submission:
	// - command buffers can be hooked (to allow us inserting/removing/changing
	//   commands), meaning we use an internal command buffer as replacement
	// - we add our own semaphore to every submission to allow tracking
	//   when they are finished (we can use that in the gui to use the resources
	//   ourselves without having to wait on cpu for the submissions to complete)
	std::vector<VkSubmitInfo> submitInfos;
	std::vector<std::vector<VkSemaphore>> semaphores;
	std::vector<std::vector<VkPipelineStageFlags>> waitStages;
	std::vector<std::vector<VkCommandBuffer>> commandBuffers;
	std::vector<std::unique_ptr<std::byte[]>> copiedChains;

	std::vector<std::vector<u64>> tsValues;
	std::deque<VkTimelineSemaphoreSubmitInfo> tsSubmitInfos;

	Device* dev;
	Queue* queue;
	SubmissionBatch* dstBatch;
	u32 submitID;

	// When we don't have timeline semaphores, we remember the gui draw we
	// used for gui synchronization so we can reset futureSemaphoreUsed
	// on error.
	Draw* syncedGuiDraw {};
};

void process(QueueSubmitter&, const VkSubmitInfo&);
void cleanupOnError(QueueSubmitter& subm);
VkResult addGuiSyncLocked(QueueSubmitter&);
void postProcessLocked(QueueSubmitter&);

} // namespace vil

