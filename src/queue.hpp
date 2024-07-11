#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <sync.hpp>
#include <util/intrusive.hpp>
#include <vk/vulkan.h>
#include <vector>
#include <optional>
#include <memory>
#include <atomic>
#include <variant>

namespace vil {

// Queue handles can't be created or destroyed. They are existent
// for the entire time the Device exists.
struct Queue : Handle {
	static constexpr auto objectType = VK_OBJECT_TYPE_QUEUE;

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
	// Also used for sparse binding submissions (even though sparse binds
	// aren't implicitly ordered with other submissions, the semaphore
	// signal operations are. Therefore we can guarantee that the value
	// never decreases even for out-of-order overlapping of bind/submit).
	VkSemaphore submissionSemaphore {};

	// Only valid when using timeline semaphores, used for full-sync.
	// The submissionID of the last submission where the layer inserted
	// commands, e.g. for gui rendering or via a CommandHook.
	u64 lastLayerSubmission {};

	// First inactive command submission to this queue. Per submission order,
	// all following (command) submissions are inactive (waiting) as well.
	// Null if there is no such submission.
	// Note that this completely ignores QueueBindSparse submissions,
	// they are never blocked by submission order.
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
	std::vector<AccelStructStatePtr> accelStructCopies; // size() == cb->rec->accelStructCopies.size()

	SubmittedCommandBuffer();
	SubmittedCommandBuffer(SubmittedCommandBuffer&&) noexcept = default;
	SubmittedCommandBuffer& operator=(SubmittedCommandBuffer&&) noexcept = default;
	~SubmittedCommandBuffer();
};

// The naming here in the vulkan spec is a bit weird:
// pending vkQueueBindSparse operations are also called "submissions" and
// there is no special name for vkQueueSubmit ops.
enum class SubmissionType : u8 {
	command, // vkQueueSubmit(2)
	bindSparse, // vkQueueBindSparse
};

// We want to keep the reference DeviceMemory objects here alive so we
// can display the submissions later in the gui.
template<typename MemBind>
struct IntrusiveMemPtrBind {
	MemBind bind;
	IntrusivePtr<DeviceMemory> mem;
};

struct SparseBufferBind {
	IntrusivePtr<Buffer> dst {};
	std::vector<IntrusiveMemPtrBind<OpaqueSparseMemoryBind>> binds;
};

struct SparseOpaqueImageBind {
	IntrusivePtr<Image> dst {};
	std::vector<IntrusiveMemPtrBind<OpaqueSparseMemoryBind>> binds;
};

struct SparseImageBind {
	IntrusivePtr<Image> dst {};
	std::vector<IntrusiveMemPtrBind<ImageSparseMemoryBind>> binds;
};

struct BindSparseSubmission {
	std::vector<SparseBufferBind> buffer;
	std::vector<SparseOpaqueImageBind> opaqueImage;
	std::vector<SparseImageBind> image;

	BindSparseSubmission();
	~BindSparseSubmission();

	BindSparseSubmission(const BindSparseSubmission&) = default;
	BindSparseSubmission& operator=(const BindSparseSubmission&) = default;
	BindSparseSubmission(BindSparseSubmission&&) noexcept = default;
	BindSparseSubmission& operator=(BindSparseSubmission&&) noexcept = default;
};

struct CommandSubmission {
	// The CommandBuffer record must stay valid while the submission
	// is still pending (anything else is an application error).
	std::vector<SubmittedCommandBuffer> cbs;
};

// A single Submission done via one VkSubmitInfo in vkQueueSubmit.
struct Submission {
	SubmissionBatch* parent {};
	u64 queueSubmitID {};

	std::vector<std::unique_ptr<SyncOp>> waits;
	std::vector<std::unique_ptr<SyncOp>> signals;

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

	// parent->type determines which one is active.
	std::variant<CommandSubmission, BindSparseSubmission> data;
};

void activateLocked(Submission& subm);
bool checkActivateLocked(Submission& subm);

// Checks all pending submissions of the given device, processing
// the finished ones. Must only be called while dev mutex is locked.
void checkPendingSubmissionsLocked(Device&);

// Implemented in submit.cpp
bool potentiallyWritesLocked(const Submission&, const Image*, const Buffer*);
std::vector<const Submission*> needsSyncLocked(const SubmissionBatch&, const Draw&);
VkResult submitSemaphore(Queue&, VkSemaphore, bool timeline = false);
VkSemaphore getSemaphoreFromPool(Device& dev);
VkSemaphore getSemaphoreFromPoolLocked(Device& dev);
VkFence getFenceFromPool(Device& dev);

// Batch of Submissions, represents and tracks one vkQueueSubmit call.
// Immutable after creation.
struct SubmissionBatch {
	Queue* queue {};
	SubmissionType type; // determines the type of objects in 'submissions'
	std::vector<Submission> submissions;
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
