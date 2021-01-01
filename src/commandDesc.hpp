#pragma once

#include <fwd.hpp>
#include <vector>
#include <string>
#include <functional>

namespace fuen {

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
	// To synchronize with command buffer resetting, the caller likely
	// has to lock the device mutex (to make sure cb doesn't get reset
	// while this is executed).
	// static std::vector<CommandDesc> get(const Command& root, const Command& cmd);
	static std::vector<CommandDesc> get(const Command& root, span<const Command*> hierachy);

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

	// Returns how much the given command chain matches this description.
	// The return value is always between 0 and 1 where 1 means a 100% match
	// and 0% means that the commands have absolutely nothing in common
	// with this description. A good empirical threshold for considering
	// a command buffer similar to a description is ~0.6.
	// float match(const Command* commands);

	friend float match(const CommandBufferDesc& a, const CommandBufferDesc& b);
};

} // namespace fuen
