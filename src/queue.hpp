#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <intrusive.hpp>
#include <commandDesc.hpp>
#include <vulkan/vulkan.h>
#include <vector>
#include <optional>
#include <memory>
#include <atomic>

namespace fuen {

// Queue handles can't be created or destroyed. They are existent
// for the entire time the Device exists.
struct Queue : Handle {
	Device* dev {};

	VkQueue handle {};
	u32 family {};
	float priority {};

	// All groups associated with this queue
	std::vector<CommandBufferGroup*> groups;

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
	FinishPtr<CommandHookSubmission> hook {};
};

struct Submission {
	std::vector<std::pair<VkSemaphore, VkPipelineStageFlags>> waitSemaphores;
	std::vector<VkSemaphore> signalSemaphores;

	// The CommandBuffer record must stay valid while the submission
	// is still pending (anything else is an application error).
	std::vector<SubmittedCommandBuffer> cbs;

	// We always add a signal semaphore to a submission, from the
	// devices semaphore pool.
	VkSemaphore ourSemaphore {};
};

struct PendingSubmission {
	Queue* queue {};
	std::vector<Submission> submissions;

	// The fence added by the caller.
	// Might be null
	Fence* appFence {};

	// When the caller didn't add a fence, we added this one from the fence pool.
	// When appFence is not null, this is null.
	VkFence ourFence {};
};

/*
// Commandbuffer hook that allows us to forward a modified version
// of this command buffer down the chain. Only called during submission,
// when the given CommandBuffer has a valid recording.
struct CommandHook {
	virtual ~CommandHook() = default;

	// Called from inside QueueSubmit with the command buffer the hook has
	// been installed for. Can therefore expect the command buffer to be
	// in executable state.
	// Can return the command buffer handle itself or a hooked one but
	// must return a valid command buffer.
	// Called while device mutex is locked.
	// Additionally has the possibility to return something in 'data' that
	// gets associated with the lifetime of the submission (i.e. is destroyed
	// when the submission is finished).
	virtual VkCommandBuffer hook(CommandBuffer& hooked,
		FinishPtr<CommandHookSubmission>& data) = 0;

	// Called when hook is removed from command buffer or command group.
	// Called while device mutex is locked.
	// Might delete itself (or decrement reference count or something).
	virtual void finish() noexcept = 0;
};

struct CommandHookSubmission {
	virtual ~CommandHookSubmission() = default;

	// Called when the submission is finished.
	// Called while device mutex is locked.
	// Might delete itself (or decrement reference count or something).
	virtual void finish() noexcept = 0;
};

struct CommandHookRecord {
	virtual ~CommandHookRecord() = default;

	// Called when associated record is destroyed or hook replaced.
	// Called while device mutex is locked.
	// Might delete itself (or decrement reference count or something).
	virtual void finish() noexcept = 0;
};
*/

// CommandBuffer groups
struct CommandBufferGroup {
	CommandBufferDesc desc;
	IntrusivePtr<CommandRecord> lastRecord;

	// All Queues this command buffer group was ever used on.
	// Also stores the last submission ID a submission with this group
	// was used on the given queue (used for cleanup).
	// Note that all queues will have the same queue family.
	std::vector<std::pair<Queue*, u64>> queues {};

	// Can be used to hook all command buffers in this group.
	FinishPtr<CommandHook> hook;
};

// Expects dev.mutex to be locked.
// If the given submission was finished and therefore
// removed, returns the iterator to the following pending submission.
// Otherwise returns nullopt.
using SubmIterator = std::vector<std::unique_ptr<PendingSubmission>>::iterator;
std::optional<SubmIterator> checkLocked(PendingSubmission& subm);

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

} // namespace fuen
