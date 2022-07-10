#pragma once

#include <rp.hpp>
#include <command/desc.hpp>
#include <command/commands.hpp>
#include <util/span.hpp>

#include <string>
#include <variant>

namespace vil::rd {

// The idea behind these more abstract CommandRecord descriptions:
// Mainly faster and better matching.

struct Label;
struct RenderPass;
struct ExecuteCommands;

using Child = std::variant<Label, RenderPass, ExecuteCommands>;

enum class BlockType {
	label, // struct Label
	renderPass, // struct RenderPass
	executeCommands, // struct ExecuteCommands
	draw, // struct BasicBlock
	transfer, // struct BasicBlock
	compute, // struct BasicBlock
};

struct CommandSpan {
	Command* begin;
	Command* end;
};

struct Block {
	BlockType type;
	CommandSpan span;
	Block* next;
};

struct BasicBlock : Block {
	u32 numCommands {}; // of own type
	u32 numSyncCommands {};
	u32 numQueryCommands {};

	// only selected handles are added e.g. pipelines, vertex/index/indirect
	// buffers, events, handles on which barriers are done, query pools
	std::vector<Handle*> usedHandles;
};

struct Level : Block {
	Block* blocks; // forward linked list
};

struct Label : Level {
	std::string name;
};

struct RenderPass {
	std::shared_ptr<RenderPassDesc> desc;
	std::vector<Level> subpasses;
};

struct ExecuteCommands {
	std::vector<Level> commandBuffers;
};

struct RecordDesc : Level {};

struct DescResult {
	RecordDesc desc;
	std::vector<Level*> dst;
};

DescResult desc(Command* commands, Command* dst = nullptr);

struct MatchResult {
	float match; // factor in [0, 1] specifying how many blocks matched
	std::vector<std::pair<Block*, Block*>> matches;
};

MatchResult match(const RecordDesc& a, const RecordDesc& b);

struct CommandDesc {
	Command* command;
	std::vector<Level*> parents;

	// saved data
	std::vector<Handle*> descriptors;
};

// Tries to find a command like 'dst' in the given block.
// The given block must have a type matching dst, i.e. if dst is a draw
// command, block must be of CommandType::draw.
Command* match(const Block& srcBlock, const Block& dstBlock,
	Command& dstCmd, span<const Handle*> dstDescriptors = {});


inline bool isActionCommand(Command& cmd) {
	// special cases
	if(dynamic_cast<const BeginDebugUtilsLabelCmd*>(&cmd)) {
		return true;
	}

	return cmd.type() == CommandType::dispatch ||
		cmd.type() == CommandType::buildAccelStruct ||
		cmd.type() == CommandType::draw ||
		// Not sure about this one. Could improve matching
		// cmd.type() == CommandType::query ||
		cmd.type() == CommandType::traceRays ||
		cmd.type() == CommandType::transfer;
}

// let's assume we have this.
Command* getPrev(Command& cmd);

inline std::pair<Command*, Command*> findContext(Command& cmd) {
	auto prev = getPrev(cmd);
	while(prev && !isActionCommand(*prev)) {
		prev = getPrev(*prev);
	}

	auto next = cmd.next;
	while(next && !isActionCommand(*next)) {
		next = next->next;
	}

	return {prev, next};
}

inline float matchContext(Command& a, Command& b) {
	// TODO: consider context across submission boundaries
	auto c1 = findContext(a);
	auto c2 = findContext(b);

	// TODO: consider additional data for matching below, e.g.
	// descriptorSets
	Matcher m;

	if(c1.first && c2.first) {
		m = c1.first->match(*c2.first);
	}

	if(c1.second && c2.second) {
		auto m2 = c1.second->match(*c2.second);
		// really add? or multiply match?
		m.match += m2.match;
		m.total += m2.total;
	}

	return eval(m);
}

} // namespac vil::rd
