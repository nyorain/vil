#pragma once

#include <fwd.hpp>
#include <vector>
#include <string>
#include <functional>

namespace vil {

// Descrption of a command relative to the current recorded state.
// Can be useful to implement heuristics identifying structurally
// similar commands in related command buffer recordings (e.g. when
// a command buffer is re-recorded or when comparing per-swapchain-image
// command buffers).
struct CommandDesc {
	// Name of the command itself. Should not contain any arguments
	// (except maybe in special cases where they modify the inherent
	// meaning of the command so far that two commands with different
	// arguments can't be considered similar).
	std::string command;
	// The most relevant arguments of this command, might be empty.
	std::vector<std::string> arguments;

	// How many commands with the same command came before this one
	// and have the same parent(s)
	u32 id {};
	// The total number of command that have same command and parent(s)
	u32 count {};

	// Expects the given command buffer to be in executable/pending state.
	static std::vector<CommandDesc> get(const Command& root, span<const Command*> hierarchy);

	static std::vector<Command*> findHierarchy(Command* root, span<const CommandDesc> desc);
	static Command* find(Command* root, span<const CommandDesc> desc);
};

// Rough structure of a command buffer recording.
struct CommandBufferDesc {
	std::string name; // command/label name information

	// statistics about number of command types
	u32 transferCommands {};
	u32 drawCommands {};
	u32 dispatchCommands {};
	u32 syncCommands {};
	u32 queryCommands {};
	u32 totalCommands {};

	std::vector<CommandBufferDesc> children;

	// Returns a description for the given set of commands.
	// The toplevel CommandBufferDesc will always be named "root".
	// Also annotates the commands, i.e. fills in Command::relID.
	// static CommandBufferDesc get(const Command* commands);
	static CommandBufferDesc getAnnotate(Command* commands);

	// Returns how similar the two given command buffer descriptions are.
	// The returned value is always between 0 and 1 where 1 means a 100% match
	// and 0% means that the descriptions have absolutely nothing in common.
	// An empirical threshold for considering two command buffers similar
	// is ~0.6-0.9.
	friend float match(const CommandBufferDesc& a, const CommandBufferDesc& b);
};

} // namespace vil
