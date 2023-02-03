#pragma once

#include <fwd.hpp>
#include <vector>
#include <memory>
#include <deque>
#include <optional>
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
	span<VkSubmitInfo2> submitInfos; // for command submit
	span<VkBindSparseInfo> bindSparseInfos; // for sparse bind

	ThreadMemScope memScope;

	Device* dev;
	Queue* queue;
	std::unique_ptr<SubmissionBatch> dstBatch;
	u64 globalSubmitID; // dev.submissionCounter
	VkFence submFence {}; // forward handle to be used on submission

	// Will be set to the local submission id of the last submission that
	// contains a command buffer hooked by us.
	std::optional<Submission*> lastLayerSubmission {};

	// When we don't have timeline semaphores, we remember the gui draw we
	// used for gui synchronization so we can reset futureSemaphoreUsed
	// on error.
	Draw* syncedGuiDraw {};
};

void init(QueueSubmitter&, Queue&, SubmissionType, VkFence);

// Processes the given submit infos, adding them to the submitter.
void process(QueueSubmitter&, span<const VkSubmitInfo2>);
void process(QueueSubmitter&, span<const VkBindSparseInfo>);

// Adds our own signal operation to each submission in the given QueueSubmitter,
// so we have a semaphore knowing when it's ready.
void addSubmissionSyncLocked(QueueSubmitter& subm);

// Makes sure the submissions synchronize properly with the gui.
void addGuiSyncLocked(QueueSubmitter&);

// Makes sure the submissions synchronize properly with any other
// layer submission (or hooked application submission).
void addFullSyncLocked(QueueSubmitter&);

void postProcessLocked(QueueSubmitter&);
void cleanupOnErrorLocked(QueueSubmitter& subm);

// = ext conversion =
VkSubmitInfo2 upgrade(Device&, ThreadMemScope& tms, const VkSubmitInfo&);
VkSubmitInfo downgrade(Device&, ThreadMemScope& tms, const VkSubmitInfo2&);

} // namespace vil

