#pragma once

// TODO: clean up. And rename to command/match.hpp?
// It's not so much about descriptions, it's about matching commands/records.
// And add more, proper documentation.

#include <fwd.hpp>
#include <ds.hpp>
#include <vector>
#include <string>
#include <functional>

namespace vil {

struct FindResult {
	std::vector<const Command*> hierachy;
	float match;
};

// Assumes that 'root' is a command tree where all handles are still valid.
FindResult find(const Command* srcRoot, span<const Command*> dstHierachyToFind,
	const CommandDescriptorSnapshot& dstDescriptors, float threshold = 0.0);

// Rough structure of a command buffer recording.
struct CommandBufferDesc {
	// Command/label name information. Name and params match exactly for two
	// sections to be considered a match
	std::string name;
	std::vector<std::string> params;

	// statistics about number of command types
	u32 transferCommands {};
	u32 drawCommands {};
	u32 dispatchCommands {};
	u32 syncCommands {};
	u32 queryCommands {};
	u32 totalCommands {};
	u32 rayTraceCommands {};

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

// WIP
struct RecordBatch;

struct RecordMatch {
	float match {};
	const CommandRecord* a {};
	const CommandRecord* b {};
};

struct BatchMatch {
	float match {};
	const RecordBatch* a {};
	const RecordBatch* b {};
	std::vector<RecordMatch> matches;
};

struct MatchResult {
	float match {};
	std::vector<BatchMatch> matches;
};

MatchResult match(span<const RecordBatch> a, span<const RecordBatch> b);

} // namespace vil
