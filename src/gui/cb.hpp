#pragma once

#include <fwd.hpp>
#include <queue.hpp>
#include <commandDesc.hpp>
#include <record.hpp>

namespace fuen {

// struct TimeCommandHook;
// struct IndirectCommandHook;
struct CommandHookImpl;

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

	// TimeCommandHook* timeHook_ {};
	// IndirectCommandHook* indirectHook_ {};
	CommandHookImpl* hook_ {};
};

} // namespace fuen
