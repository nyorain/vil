#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <commandDesc.hpp>
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

	// All groups associated with this queue
	std::unordered_set<CommandBufferGroup*> groups;

	// Counted up each time this queue is submitted to.
	// Might wrap around.
	u64 submissionCount {};
};

// All data we store for a queue family.
struct QueueFamily {
	VkQueueFamilyProperties props;

	// Resettable command pool.
	VkCommandPool commandPool {};

	// Command groups used in this queue family.
	std::vector<std::unique_ptr<CommandBufferGroup>> commandGroups;
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
	SubmissionBatch* parent {};

	std::vector<std::pair<Semaphore*, VkPipelineStageFlags>> waitSemaphores;
	std::vector<Semaphore*> signalSemaphores;

	// The CommandBuffer record must stay valid while the submission
	// is still pending (anything else is an application error).
	std::vector<SubmittedCommandBuffer> cbs;

	// We always add a signal semaphore to a submission, from the
	// devices semaphore pool. When we have timeline semaphores, we also
	// store the associated value this semaphore is set to by this submission.
	VkSemaphore ourSemaphore {};
	u64 ourSemaphoreValue {};
};

bool potentiallyWritesLocked(Submission&, DeviceHandle&);
std::unordered_set<Submission*> needsSyncLocked(SubmissionBatch&, Draw&);

// Batch of Submissions, represents and tracks one vkQueueSubmit call.
struct SubmissionBatch {
	Queue* queue {};
	std::vector<Submission> submissions; // immutable after creation

	// The fence added by the caller.
	// Might be null
	Fence* appFence {};

	// When the caller didn't add a fence, we added this one from the fence pool.
	// When appFence is not null, this is null.
	VkFence ourFence {};
};

// CommandBuffer groups
// TODO: rename to CommandGroup? CommandRecordGroup? RecordGroup?
struct CommandBufferGroup {
	// Abstract description of this group. Used to determine whether new
	// recording belong to it.
	CommandBufferDesc desc;

	// A list of all representatives of this group that are alive.
	// Synchronized via dev.mutex. As long as more than one CommandRecord
	// of this group is still alive, it is not being destroyed.
	// (One is always kept alive via lastRecord, that does not count
	// as it would create a lifetime circle, never causing us to free
	// command buffer groups).
	std::unordered_set<CommandRecord*> aliveRecords;

	// We make sure at least the last representative of this command buffer
	// group is not being destroyed. This allows to e.g. inspect it in gui.
	IntrusivePtr<CommandRecord> lastRecord;

	// All Queues this command buffer group was ever used on.
	// Also stores the last submission ID a submission with this group
	// was used on the given queue (used for cleanup).
	// Note that all queues will have the same queue family.
	std::vector<std::pair<Queue*, u64>> queues {};

	CommandBufferGroup();
	~CommandBufferGroup();
};

// Expects dev.mutex to be locked.
// If the given submission was finished and therefore
// removed, returns the iterator to the following pending submission.
// Otherwise returns nullopt.
using SubmIterator = std::vector<std::unique_ptr<SubmissionBatch>>::iterator;
std::optional<SubmIterator> checkLocked(SubmissionBatch& subm);

// api
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(
	VkQueue                                     queue,
	uint32_t                                    submitCount,
	const VkSubmitInfo*                         pSubmits,
	VkFence                                     fence);

VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(
    VkQueue                                     queue);

VKAPI_ATTR VkResult VKAPI_CALL DeviceWaitIdle(
    VkDevice                                    device);

} // namespace vil
