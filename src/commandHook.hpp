#pragma once

#include <fwd.hpp>
#include <span.hpp>
#include <vulkan/vulkan.h>
#include <memory>
#include <vector>

namespace fuen {

struct CommandHookSubmissionImpl;
struct CommandHookRecordImpl;
struct CommandHookImpl;

struct CommandHookSubmission;
struct CommandHookRecord;
struct CommandHook;

// Enum specifies how command buffer hooking should be handled for command
// buffers that are submitted more than once at a time.
enum class SimultaneousSubmitHook {
	// Allow it, i.e. simply submit the recorded hooked command buffer
	// then twice as well.
	allow,
	// Skip the hooking in that case, the submission will simply contain the
	// original command buffer. Mainly used by readback-hooks that
	// do lose information in that situation but don't care too much as
	// there is still a command buffer generating new information pending.
	skip,
	// Create a new recording in that case. This one is expensive but
	// has to be chosen by hooks that need to be present in *all* submissions
	// and cannot guarantee simulataneous submission of their changes.
	recreate,
};

struct CommandHook {
public:
	~CommandHook();
	VkCommandBuffer hook(CommandBuffer& hooked, CommandHookSubmission& subm);

	void add(std::unique_ptr<CommandHookImpl> impl);
	void remove(CommandHookImpl& impl);

	void desc(std::vector<CommandDesc> desc);
	span<const CommandDesc> desc() const { return desc_; }
	void invalidateRecordings();
	bool active() const { return !impls_.empty(); }

	// helpers
	void recordHook(CommandHookRecord& record, Command* cmd, Command& hooked);
	void fillSubmission(CommandHookRecord& record, CommandHookSubmission& subm);

private:
	std::vector<std::unique_ptr<CommandHookImpl>> impls_;
	u32 counter_ {};
	std::vector<CommandDesc> desc_ {};

	friend struct CommandHookRecord; // See it's destructor
	CommandHookRecord* records_ {}; // Linked list
};

struct CommandHookRecord {
	std::vector<std::unique_ptr<CommandHookRecordImpl>> impls;

	CommandHook* hook {};
	CommandRecord* record {};
	u32 hookCounter {};

	// NOTE: could make this linked list of submissions instead
	u32 submissionCount {};
	SimultaneousSubmitHook simulataneousBehavior;

	VkCommandBuffer cb {};

	// linked list of records
	CommandHookRecord* next {};
	CommandHookRecord* prev {};

	void finish() noexcept;// called when unset in associated CommandRecord

	CommandHookRecord() = default;
	~CommandHookRecord();

	CommandHookRecord(const CommandHookRecord&) = delete;
	CommandHookRecord& operator=(const CommandHookRecord&) = delete;
};

struct CommandHookSubmission {
	std::vector<std::unique_ptr<CommandHookSubmissionImpl>> impls;
	CommandHookRecord* record {};

	CommandHookSubmission() = default;
	~CommandHookSubmission();

	CommandHookSubmission(const CommandHookSubmission&) = delete;
	CommandHookSubmission& operator=(const CommandHookSubmission&) = delete;
};

// Impls
struct CommandHookSubmissionImpl {
	// Called when the submission is finished.
	// It will be destroyed after this.
	virtual void finish(Device&) noexcept {}
	virtual ~CommandHookSubmissionImpl() = default;
};

struct CommandHookRecordImpl {
	// The next record hook in the chain. Usually used by 'record' implementations.
	// Always != null for implementations.
	CommandHookRecordImpl* next {};

	// Recording: called only when a new record is created, initially.
	virtual void recordBeforeHooked(Device&, VkCommandBuffer, Command&) {}
	// virtual void recordAfterHooked(Device&, VkCommandBuffer, Command&) {}
	// virtual void recordBeforeHookedChildren(Device&, VkCommandBuffer, Command&) {}
	virtual void recordAfterHookedChildren(Device&, VkCommandBuffer, Command&) {}

	virtual void record(Device& dev, VkCommandBuffer cb,
			Command& toBeRecorded, Command& hooked) {
		next->record(dev, cb, toBeRecorded, hooked);
	}

	// Called every time a new submission with this recording is done.
	virtual std::unique_ptr<CommandHookSubmissionImpl> submit() { return nullptr; }
	virtual SimultaneousSubmitHook simulataneousBehavior() const {
		return SimultaneousSubmitHook::allow;
	}

	// Called before the recording is destroyed.
	virtual void finish(Device&) noexcept {}

	// Called when the record is invalidated. It should no longer connect
	// back to its hook, cannot assume its existence anymore.
	virtual void invalidate() {}

	virtual ~CommandHookRecordImpl() = default;
};

struct CommandHookImpl {
	virtual ~CommandHookImpl() = default;
	virtual std::unique_ptr<CommandHookRecordImpl> createRecord(
		CommandBuffer&, Command& hooked) = 0;

	// The order in which the hook is inserted.
	// Lower values come first, before the other hooks.
	// Must be constant throughout the lifetime of the object.
	virtual int order() const { return 0; }
};

} // namespace fuen
