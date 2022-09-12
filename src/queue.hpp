#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <util/intrusive.hpp>
#include <vk/vulkan.h>
#include <vector>
#include <optional>
#include <memory>
#include <atomic>

namespace vil {

// Queue handles can't be created or destroyed. They are existent
// for the entire time the Device exists.
struct Queue : Handle {
	Device* dev {};

	VkQueue handle {};
	u32 family {};
	float priority {};

	// Whether the queue was created by us, for internal use.
	bool createdByUs {};

	// Counted up each time this queue is submitted to.
	// Might wrap around.
	u64 submissionCounter {};

	// Only valid when using timeline semaphores, used for full-sync.
	// Semaphore that will get set by each submission to this queue to
	// their corresponding submissionID.
	// When its current value is same as submissionCounter,
	// there are no pending submissions on this queue.
	VkSemaphore submissionSemaphore {};

	// Only valid when using timeline semaphores, used for full-sync.
	// The submissionID of the last submission where the layer inserted
	// commands, e.g. for gui rendering or via a CommandHook.
	u64 lastLayerSubmission {};

	// First inactive submission to this queue. Per submission order,
	// all following submissions are inactive (waiting) as well.
	// Null if there is no such submission.
	Submission* firstWaiting {};
};

// All data we store for a queue family.
struct QueueFamily {
	VkQueueFamilyProperties props;

	// Resettable command pool.
	VkCommandPool commandPool {};
};

struct SubmittedCommandBuffer {
	CommandBuffer* cb {};
	std::unique_ptr<CommandHookSubmission> hook; // optional

	SubmittedCommandBuffer();
	SubmittedCommandBuffer(SubmittedCommandBuffer&&) noexcept = default;
	SubmittedCommandBuffer& operator=(SubmittedCommandBuffer&&) noexcept = default;
	~SubmittedCommandBuffer();
};

// A single Submission done via one VkSubmitInfo in vkQueueSubmit.
struct Submission {
	struct Sync {
		Semaphore* semaphore {};
		VkPipelineStageFlags stages {};
		u64 value {1u}; // always 1 for binary sems
	};

	SubmissionBatch* parent {};
	u64 queueSubmitID {};

	std::vector<Sync> waits;
	std::vector<Sync> signals;

	// The CommandBuffer record must stay valid while the submission
	// is still pending (anything else is an application error).
	std::vector<SubmittedCommandBuffer> cbs;

	// When not having timeline semaphores, we always add a binary
	// semaphore to the submission to allow chaining it with future
	// submissions. Otherwise the per-queue timeline semaphore can
	// simply be used (with this->queueSubmitID as value).
	VkSemaphore ourSemaphore {};

	// Whether this submission might be executed on the device right now.
	// Sometimes submissions are logically dependent on something not
	// yet submitted yet, in which case active is false until all
	// dependencies are submitted (i.e. 'waits' theoretically satisfied).
	// Synced via device mutex
	bool active {};
};

void activateLocked(Submission& subm);
bool checkActivateLocked(Submission& subm);

// Checks all pending submissions of the given device, processing
// the finished ones. Must only be called while dev mutex is locked.
void checkPendingSubmissionsLocked(Device&);

// Implemented in submit.cpp
bool potentiallyWritesLocked(const Submission&, const DeviceHandle&);
std::vector<const Submission*> needsSyncLocked(const SubmissionBatch&, const Draw&);
VkResult submitSemaphore(Queue&, VkSemaphore, bool timeline = false);
VkSemaphore getSemaphoreFromPool(Device& dev);
VkSemaphore getSemaphoreFromPoolLocked(Device& dev);
VkFence getFenceFromPool(Device& dev);

// Batch of Submissions, represents and tracks one vkQueueSubmit call.
struct SubmissionBatch {
	Queue* queue {};
	std::vector<Submission> submissions; // immutable after creation
	u64 globalSubmitID {};

	// The fence added by the caller.
	// Might be null
	Fence* appFence {};

	// When the caller didn't add a fence, we added this one from the fence pool.
	// When appFence is not null, this is null.
	VkFence ourFence {};

	// Device pool semaphores that should be re-added to the pool after this.
	// Only currently used when timeline semaphores aren't available.
	std::vector<VkSemaphore> poolSemaphores {};
};

// Expects dev.mutex to be locked.
// If the given submission was finished and therefore
// removed, returns the iterator to the following pending submission.
// Otherwise returns nullopt.
using SubmIterator = std::vector<std::unique_ptr<SubmissionBatch>>::iterator;
std::optional<SubmIterator> checkLocked(SubmissionBatch& subm);

VkResult waitIdleImpl(Device& dev);
VkResult doSubmit(Queue& qd, span<const VkSubmitInfo2> submits,
	VkFence fence, bool legacy);

// api
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(
	VkQueue                                     queue,
	uint32_t                                    submitCount,
	const VkSubmitInfo*                         pSubmits,
	VkFence                                     fence);

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(
    VkQueue                                     queue,
    uint32_t                                    submitCount,
    const VkSubmitInfo2*                        pSubmits,
    VkFence                                     fence);

VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(
    VkQueue                                     queue);

VKAPI_ATTR VkResult VKAPI_CALL DeviceWaitIdle(
    VkDevice                                    device);

VKAPI_ATTR VkResult VKAPI_CALL QueueBindSparse(
    VkQueue                                     queue,
    uint32_t                                    bindInfoCount,
    const VkBindSparseInfo*                     pBindInfo,
    VkFence                                     fence);

} // namespace vil
