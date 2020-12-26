#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <intrusive.hpp>
#include <commandDesc.hpp>
#include <vulkan/vulkan.h>
#include <vector>
#include <optional>
#include <memory>

namespace fuen {

struct CommandBufferGroup;
struct CommandBufferRecord;

struct Queue : Handle {
	Device* dev {};

	VkQueue handle {};
	VkQueueFlags flags {};
	u32 family {};
	float priority {};

	std::vector<std::unique_ptr<CommandBufferGroup>> groups;
};

struct Submission {
	std::vector<std::pair<VkSemaphore, VkPipelineStageFlags>> waitSemaphores;
	std::vector<VkSemaphore> signalSemaphores;
	std::vector<CommandBuffer*> cbs;

	// We always add a signal semaphore to a submission, from the
	// devices semaphore pool.
	VkSemaphore ourSemaphore;
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

// CommandBuffer groups
struct CommandBufferGroup {
	IntrusivePtr<CommandBufferRecord> lastRecord;

	std::vector<CommandBuffer*> cbs;

	Queue* queue {};
	CommandBufferDesc desc; // nested tree
	u32 submissions {};
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
