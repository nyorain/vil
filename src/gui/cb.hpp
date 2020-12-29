#pragma once

#include <device.hpp>
#include <queue.hpp>
#include <commandDesc.hpp>
#include <boundState.hpp>

namespace fuen {

struct TimeCommandHookSubmission;
struct TimeCommandHookRecord;

// Time hooking
struct TimeCommandHook : CommandHook {
	u64 lastTime {};
	u32 refCount {0};
	u32 counter {0};
	std::vector<CommandDesc> desc {};
	TimeCommandHookRecord* records {}; // linked list

	VkCommandBuffer hook(CommandBuffer& hooked,
		FinishPtr<CommandHookSubmission>& data) override;
	void finish() noexcept override;
	~TimeCommandHook();
};

struct TimeCommandHookRecord : CommandHookRecord {
	TimeCommandHook* hook {};
	u32 hookCounter {};
	CommandRecord* record {};

	VkCommandBuffer cb {};
	VkQueryPool queryPool {}; // TODO: allocate from pool
	u32 refCount {0};

	// linked list of records
	TimeCommandHookRecord* next {};
	TimeCommandHookRecord* prev {};

	~TimeCommandHookRecord();
	void hookRecord(Device& dev, Command* cmd, Command* hooked);
	void finish() noexcept override;
};

struct TimeCommandHookSubmission : CommandHookSubmission {
	IntrusivePtr<TimeCommandHookRecord> record;

	TimeCommandHookSubmission(TimeCommandHookRecord& rec) : record(&rec) {}
	~TimeCommandHookSubmission();
	void finish() noexcept override { delete this; }
};

struct CommandBufferGui {
	void draw();
	void select(CommandBufferGroup& group);
	void select(IntrusivePtr<CommandRecord> record);
	void destroyed(const Handle& handle);

	CommandBufferGui() = default;
	~CommandBufferGui() = default;

	Gui* gui_ {};
	bool updateFromGroup_ {};

	// The command record we are currently viewing.
	// We keep it alive.
	IntrusivePtr<CommandRecord> record_ {};

	// The selected command inside the cb, might be null.
	const Command* command_ {};
	// In case we have a selected command, we store its description inside
	// the CommandRecord here. This way we can (try to) find the logically
	// same command in future records/cb selections.
	std::vector<CommandDesc> desc_ {};

	bool queryTime_ {};
	IntrusivePtr<TimeCommandHook> timeHook_ {};
};

} // namespace fuen
