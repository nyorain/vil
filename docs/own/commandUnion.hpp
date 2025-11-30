#pragma once

#include <command/record.hpp>
#include <command/commands.hpp>
#include <command/match.hpp>

// TODO
// - what about out-of-order submission via timeline semaphores?
//   should probably order in FrameSubmissions by activation time, not submission time.

// meh this is too complicated: we would need to match *all* commands, not just
// section commands. Too expensive, unrealistic.

namespace vil {

struct GroupCommandInfo {
	u32 lastSeen {};
};

struct CommandGroup {
	CommandRecord record;
	std::unordered_map<CommandRecord*, GroupCommandInfo> meta;
};

// combine record into this group
void unionize(CommandGroup& group, const CommandRecord& other, const CommandRecordMatch& match, u32 frameID) {
	auto* groupIt = group.record.commands->firstChildParent();
	auto* otherIt = other.commands->firstChildParent();

	for (auto& match : match.matches) {
		if (match.a ==
	}
}

} // namespace
