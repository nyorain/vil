#pragma once

#include <rp.hpp>
#include <util/span.hpp>

#include <string>
#include <variant>

namespace vil::rd {

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
	std::vector<Block> blocks;
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

} // namespac vil::rd
