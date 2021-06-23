#pragma once

#include <fwd.hpp>
#include <vector>
#include <memory>
#include <deque>
#include <threadContext.hpp>
#include <vk/vulkan.h>
#include <util/intrusive.hpp>

// Builder operations for a queue submission.
// We have to hook/track/manage a lot of stuff when an applications
// do a queue submission and our vkQueueSubmit implementation became
// too compliated so we split it up into the smaller components here.

namespace vil {

// Allocates all internal data from its memScope. No internal data
// (e.g. submitInfo) must outlive its QueueSubmitter object.
struct QueueSubmitter {
	// Custom information for the submissions we build.
	// We might have to modify the submission:
	// - command buffers can be hooked (to allow us inserting/removing/changing
	//   commands), meaning we use an internal command buffer as replacement
	// - we add our own semaphore to every submission to allow tracking
	//   when they are finished (we can use that in the gui to use the resources
	//   ourselves without having to wait on cpu for the submissions to complete)
	span<VkSubmitInfo> submitInfos;

	ThreadMemScope memScope;

	Device* dev;
	Queue* queue;
	SubmissionBatch* dstBatch;
	u64 queueSubmitID; // queue.submissionCounter
	u64 globalSubmitID; // dev.submissionCounter

	// When we don't have timeline semaphores, we remember the gui draw we
	// used for gui synchronization so we can reset futureSemaphoreUsed
	// on error.
	Draw* syncedGuiDraw {};
};

void process(QueueSubmitter&, span<const VkSubmitInfo>);
void cleanupOnError(QueueSubmitter& subm);
void addGuiSyncLocked(QueueSubmitter&);
void postProcessLocked(QueueSubmitter&);

} // namespace vil

