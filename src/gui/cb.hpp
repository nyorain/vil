#pragma once

#include <fwd.hpp>
#include <queue.hpp>
#include <commandDesc.hpp>
#include <record.hpp>

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

struct CommandBufferGui {
	void draw();
	void select(IntrusivePtr<CommandRecord> record, bool updateFromGroup);
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
