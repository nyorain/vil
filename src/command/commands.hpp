#pragma once

#include <fwd.hpp>
#include <command/record.hpp>
#include <pipe.hpp>
#include <util/dlg.hpp>
#include <util/linalloc.hpp>
#include <nytl/flags.hpp>
#include <nytl/span.hpp>

// See ~Command. Keep in mind that we use a custom per-CommandRecord allocator
// for all Commands, that's why we can use span<> here without the referenced
// data being owned anywhere. It's just guaranteed to stay alive as long
// as the CommandRecord.
#ifdef __GNUC__
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif // __GNUC__

namespace vil {

// The type of a command is used e.g. to hide them in the UI.
enum class CommandCategory : u32 {
	other = (1u << 0u),
	// Commands that bind state, buffers, push constants etc
	bind = (1u << 1u),
	// States that are responsible for synchronization, events,
	// barriers etc.
	sync = (1u << 2u),
	// Draw commands
	draw = (1u << 3u),
	// Dispatch commdnas
	dispatch = (1u << 4u),
	// Copies, clears and blits
	transfer = (1u << 5u),
	// Commands that end a section
	end = (1u << 6u),
	// Query pool commands
	query = (1u << 7u),
	// TracyRays commands
	traceRays = (1u << 8u),
	// BuildAccelerationStructures commands
	buildAccelStruct = (1u << 9u),
	// BeginRenderPass
	beginRenderPass = (1u << 10u),
	// RenderSectionCommand
	renderSection = (1u << 11u),
};

enum class CommandType : u32 {
	root = 0,
	waitEvents,
	waitEvents2,
	barrier,
	barrier2,
	beginRenderPass,
	nextSubpass,
	firstSubpass, // implicit
	endRenderPass,
	draw,
	drawIndirect,
	drawIndexed,
	drawIndirectCount,
	drawMulti,
	drawMultiIndexed,
	bindVertexBuffers,
	bindIndexBuffer,
	bindDescriptorSet,
	dispatch,
	dispatchIndirect,
	dispatchBase, // DispatchBaseCmd NOT DispatchCmdBase
	copyImage,
	copyBufferToImage,
	copyImageToBuffer,
	blitImage,
	resolveImage,
	copyBuffer,
	updateBuffer,
	fillBuffer,
	clearColorImage,
	clearDepthStencilImage,
	clearAttachment,
	setEvent,
	setEvent2,
	resetEvent,
	executeCommandsChild, // implicit
	executeCommands,
	beginDebugUtilsLabel,
	endDebugUtilsLabel,
	insertDebugUtilsLabel,
	bindPipeline,
	pushConstants,
	setViewport,
	setScissor,
	setLineWidth,
	setDepthBias,
	setDepthBounds,
	setBlendConstants,
	setStencilCompareMask,
	setStencilWriteMask,
	setStencilReference,
	beginQuery,
	endQuery,
	resetQueryPool,
	writeTimestamp,
	copyQueryPoolResults,
	pushDescriptorSet,
	pushDescriptorSetWithTemplate,
	setFragmentShadingRate,
	beginConditionalRendering,
	endConditionalRendering,
	setLineStipple,
	setCullMode,
	setFrontFace,
	setPrimitiveTopology,
	setViewportWithCount,
	setScissorWithCount,
	setDepthTestEnable,
	setDepthWriteEnable,
	setDepthCompareOp,
	setDepthBoundsTestEnable,
	setStencilTestEnable,
	setStencilOp,
	setPatchControlPoints,
	setRasterizerDiscardEnable,
	setDepthBiasEnable,
	setLogicOp,
	setPrimitiveRestartEnable,
	setSampleLocations,
	setDisacrdRectangle,
	copyAccelStruct,
	copyAccelStructToMemory,
	copyMemoryToAccelStruct,
	writeAccelStructsProperties,
	buildAccelStructs,
	buildAccelStructsIndirect,
	traceRays,
	traceRaysIndirect,
	setRayTracingPipelineStackSize,
	beginRendering,
	endRendering,
	setVertexInput,
	setColorWriteEnable,

	count,
};

using CommandCategoryFlags = nytl::Flags<CommandCategory>;

struct CommandVisitor;

template<typename C>
void doVisit(CommandVisitor& v, C& cmd);

// The list of commands in a CommandBuffer is organized as a tree.
// Section-like commands (e.g. cmdBeginRenderPass) have all their commands
// stored as children
struct Command {
	using Type = CommandType;
	using Category = CommandCategory;
	using CategoryFlags = CommandCategoryFlags;

	Command();

	// NOTE: Commands should never have a non-trivial destructor (that is
	// static_assert'd in addCmd) since it won't be called. We do this
	// so that resetting command buffers does not add significant overhead
	// as we can have *a lot* of commands in a command buffer.
	// When special commands need resources with destructor, those should
	// be separately stored in the command record.
	~Command() = default;

	// The name of the command as string (might include parameter information).
	// Used by the default 'display' implementation.
	virtual std::string toString() const { return std::string(nameDesc()); }

	// Returns the name of the command.
	virtual std::string_view nameDesc() const = 0;

	virtual Category category() const { return Category::other; }
	virtual Type type() const = 0;

	// Records itself into the given command buffer.
	// Does not record its child commands (if it has any).
	virtual void record(const Device& dev, VkCommandBuffer, u32 qfam) const = 0;

	// Draws the command inspector UI.
	// Gets a list of destroyed handled, might include referenced handles
	// by this command. Must not dereference them.
	virtual void displayInspector(Gui&) const {}

	// Returns the children command list. For non-parent commands, this
	// is simply null.
	virtual Command* children() const { return nullptr; }

	// Returns whether the given command is a child of this one.
	// Complexity is linear in the number of child commands.
	// Returns false for itself.
	virtual bool isChild(const Command& cmd) const;

	// Returns whether the given command is a descendant of this one.
	// Complexity is linear in the number of descendants.
	// Returns false for itself.
	virtual bool isDescendant(const Command& cmd) const;

	virtual void visit(CommandVisitor& v) const = 0;

	// Forms a forward linked list with siblings
	Command* next {};

#ifdef VIL_COMMAND_CALLSTACKS
	span<void*> stacktrace {};
#endif // VIL_COMMAND_CALLSTACKS
};

NYTL_FLAG_OPS(Command::Category)

enum class NullName {
	empty, // ""
	null, // "<null>"
	destroyed, // "<destroyed>"
};

enum class NameType {
	null, // handle was null. Name depends on NullName passed to name()
	unnamed, // handle has no name. Name might contain meta information
	named, // name will contain application-given debug name
};

struct NameResult {
	NameType type;
	std::string name;
};

NameResult nameRes(Handle*, VkObjectType, NullName = NullName::null, bool displayType = false);

template<typename H>
NameResult nameRes(H* handle, NullName nn = NullName::null, bool displayType = false) {
	return nameRes(handle, handle ? handle->objectType : VK_OBJECT_TYPE_MAX_ENUM, nn, displayType);
}

// Base Command class for commands that have children.
// These children might be directly part of the command record (see SectionCommand)
// or external (e.g. ExecuteCommandBuffersCmd).
struct ParentCommand : Command {
	struct SectionStats {
		u32 numDraws {};
		u32 numDispatches {};
		u32 numRayTraces {};
		u32 numTransfers {};
		u32 numSyncCommands {};
		u32 numTotalCommands {};
		u32 numChildSections {};

		u32 totalNumCommands {};

		// Most significant used handles. Important to not include
		// anything temporary here.
		// TODO: add a BlockVector utility and use that here.
		struct BoundPipeNode {
			BoundPipeNode* next {};
			Pipeline* pipe {};
		};
		BoundPipeNode* boundPipelines {};
		u32 numPipeBinds {};
	};

	void visit(CommandVisitor& v) const override { doVisit(v, *this); }

	// Returns the stats over the section this ParentCommand represents.
	// Note that the stats only contain information about the direct children.
	virtual const SectionStats& sectionStats() const = 0;

	// First command of the direct children that is also a ParentCommand.
	// Might be null if all children are leaf commands.
	virtual ParentCommand* firstChildParent() const = 0;

	// Points to the next ParentCommand on the same level as this one.
	ParentCommand* nextParent_ {};
};

// Base Command class for all commands containing their children, such
// as BeginRenderPassCmd, BeginDebugUtilsLabelCmd or BeginConditionalRenderingCmd.
struct SectionCommand : ParentCommand {
	Command* children_ {};
	SectionStats stats_ {};
	ParentCommand* firstChildParent_ {};

	Command* children() const override { return children_; }
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
	const SectionStats& sectionStats() const override { return stats_; }
	ParentCommand* firstChildParent() const override { return firstChildParent_; }
};

template<typename B, CommandType Type>
struct CmdDerive : B {
	using B::B;

	static constexpr CommandType staticType() { return Type; }
	CommandType type() const override { return staticType(); }
};

// Meta-command. Root node of the command hierarchy.
// Empty SectionCommand.
struct RootCommand final : CmdDerive<SectionCommand, CommandType::root> {
	void record(const Device&, VkCommandBuffer, u32) const override {}
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
	Category category() const override { return Category::other; }
	std::string_view nameDesc() const override { return "<RootCommand>"; }
};

struct BarrierCmdBase : Command {
    VkPipelineStageFlags srcStageMask {}; // only set when legacy
    VkPipelineStageFlags dstStageMask {}; // only set when legacy

	span<VkMemoryBarrier> memBarriers;
	span<VkBufferMemoryBarrier> bufBarriers;
	span<VkImageMemoryBarrier> imgBarriers;

	span<Image*> images;
	span<Buffer*> buffers;

	void displayInspector(Gui& gui) const override;
	MatchVal doMatch(const BarrierCmdBase& rhs) const;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }

	// NOTE: we need to patch barriers when recording (instead of just
	// storing the patched versions) so we can still show the correct,
	// application-provided barriers in the UI.
	struct PatchedBarriers {
		span<const VkMemoryBarrier> memBarriers;
		span<VkBufferMemoryBarrier> bufBarriers;
		span<VkImageMemoryBarrier> imgBarriers;
	};

	PatchedBarriers patchedBarriers(ThreadMemScope& memScope,
		u32 qfam) const;
};

// For synchronization2 commands.
// Not merged with BarrierCmdBase so we can display them correctly
// in the UI.
struct Barrier2CmdBase : Command {
	VkDependencyFlags flags {};
	span<VkMemoryBarrier2> memBarriers;
	span<VkBufferMemoryBarrier2> bufBarriers;
	span<VkImageMemoryBarrier2> imgBarriers;

	span<Image*> images;
	span<Buffer*> buffers;

	void displayInspector(Gui& gui) const override;
	MatchVal doMatch(const Barrier2CmdBase& rhs) const;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }

	// NOTE: we need to patch barriers when recording (instead of just
	// storing the patched versions) so we can still show the correct,
	// application-provided barriers in the UI.
	VkDependencyInfo patchedBarriers(ThreadMemScope& memScope,
		u32 qfam) const;
};

struct WaitEventsCmd : CmdDerive<BarrierCmdBase, CommandType::waitEvents> {
	span<Event*> events;

	std::string_view nameDesc() const override { return "WaitEvents"; }
	Category category() const override { return Category::sync; }
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct WaitEvents2Cmd : CmdDerive<Barrier2CmdBase, CommandType::waitEvents2> {
	span<Event*> events;

	std::string_view nameDesc() const override { return "WaitEvents2"; }
	Category category() const override { return Category::sync; }
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct BarrierCmd : CmdDerive<BarrierCmdBase, CommandType::barrier> {
    VkDependencyFlags dependencyFlags {};

	std::string_view nameDesc() const override { return "PipelineBarrier"; }
	Category category() const override { return Category::sync; }
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct Barrier2Cmd : CmdDerive<Barrier2CmdBase, CommandType::barrier2> {
	std::string_view nameDesc() const override { return "PipelineBarrier2"; }
	Category category() const override { return Category::sync; }
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// All direct children must be of type 'NextSubpassCmd'
struct BeginRenderPassCmd final : CmdDerive<SectionCommand, CommandType::beginRenderPass> {
	VkRenderPassBeginInfo info {}; // TODO: remove, extract renderArea
	span<VkClearValue> clearValues;

	RenderPass* rp {};

	// Used attachments, stored here separately from framebuffer since
	// the framebuffer might be imageless
	span<ImageView*> attachments;
	Framebuffer* fb {}; // NOTE: might be null for imageless framebuffer ext

	VkSubpassBeginInfo subpassBeginInfo {}; // for the first subpass

	// Returns the subpass that contains the given command.
	// Returns u32(-1) if no subpass is ancestor of the given command.
	u32 subpassOfDescendant(const Command& cmd) const;

	std::string toString() const override;
	std::string_view nameDesc() const override { return "BeginRenderPass"; }
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
	Category category() const override { return Category::beginRenderPass; }
};

struct RenderSectionCommand : SectionCommand {
	RenderPassInstanceState rpi;
	Category category() const override { return Category::renderSection; }
};

struct SubpassCmd : RenderSectionCommand {
	u32 subpassID {};
};

struct NextSubpassCmd final : CmdDerive<SubpassCmd, CommandType::nextSubpass> {
	VkSubpassEndInfo endInfo {}; // for the previous subpass
	VkSubpassBeginInfo beginInfo; // for the new subpass

	using CmdDerive::CmdDerive;

	// toString should probably rather return the subpass number.
	// Must be tracked while recording
	std::string_view nameDesc() const override { return "NextSubpass"; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// Meta command needed for correct hierachy. We want each subpass to
// have its own section.
struct FirstSubpassCmd final : CmdDerive<SubpassCmd, CommandType::firstSubpass> {
	using CmdDerive::CmdDerive;
	std::string_view nameDesc() const override { return "Subpass 0"; }
	void record(const Device&, VkCommandBuffer, u32) const override {}
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct EndRenderPassCmd final : CmdDerive<Command, CommandType::endRenderPass> {
	VkSubpassEndInfo endInfo {}; // for the previous subpass

	std::string_view nameDesc() const override { return "EndRenderPass"; }
	Category category() const override { return Category::end; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// Base command from which all draw, dispatch and traceRays command are derived.
struct StateCmdBase : Command {
	virtual const DescriptorState& boundDescriptors() const = 0;
	virtual const Pipeline* boundPipe() const = 0;
	virtual const PushConstantData& boundPushConstants() const = 0;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct DrawCmdBase : StateCmdBase {
	const GraphicsState* state {};
	PushConstantData pushConstants;

	DrawCmdBase(); // dummy constructor
	DrawCmdBase(CommandBuffer& cb);

	Category category() const override { return Category::draw; }
	void displayGrahpicsState(Gui& gui, bool indices) const;
	MatchVal doMatch(const DrawCmdBase& cmd, bool indexed) const;

	const DescriptorState& boundDescriptors() const override { return *state; }
	const Pipeline* boundPipe() const override { return state->pipe; }
	const PushConstantData& boundPushConstants() const override { return pushConstants; }
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct DrawCmd final : CmdDerive<DrawCmdBase, CommandType::draw> {
	u32 vertexCount;
	u32 instanceCount;
	u32 firstVertex;
	u32 firstInstance;

	using CmdDerive::CmdDerive;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "Draw"; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct DrawIndirectCmd final : CmdDerive<DrawCmdBase, CommandType::drawIndirect> {
	Buffer* buffer {};
	VkDeviceSize offset {};
	u32 drawCount {};
	u32 stride {};
	bool indexed {};

	using CmdDerive::CmdDerive;

	std::string toString() const override;
	std::string_view nameDesc() const override {
		return indexed ? "DrawIndexedIndirect" : "DrawIndirect";
	}
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct DrawIndexedCmd final : CmdDerive<DrawCmdBase, CommandType::drawIndexed> {
	u32 indexCount {};
	u32 instanceCount {};
	u32 firstIndex {};
	i32 vertexOffset {};
	u32 firstInstance {};

	using CmdDerive::CmdDerive;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "DrawIndexed"; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct DrawIndirectCountCmd final : CmdDerive<DrawCmdBase, CommandType::drawIndirectCount> {
	Buffer* buffer {};
	VkDeviceSize offset {};
	u32 maxDrawCount {};
	u32 stride {};
	Buffer* countBuffer {};
	VkDeviceSize countBufferOffset {};
	bool indexed {};

	using CmdDerive::CmdDerive;

	std::string toString() const override;
	std::string_view nameDesc() const override {
		return indexed ? "DrawIndexedIndirectCount" : "DrawIndirectCount";
	}
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct DrawMultiCmd final : CmdDerive<DrawCmdBase, CommandType::drawMulti> {
	span<VkMultiDrawInfoEXT> vertexInfos;
	u32 instanceCount {};
	u32 firstInstance {};
	// NOTE: only here for gui, we don't forward it since we copied
	//   the vertexInfos into a tight span
	u32 stride {};

	using CmdDerive::CmdDerive;

	std::string toString() const override;
	std::string_view nameDesc() const override { return "DrawMulti"; };
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct DrawMultiIndexedCmd final : CmdDerive<DrawCmdBase, CommandType::drawMultiIndexed> {
	span<VkMultiDrawIndexedInfoEXT> indexInfos;
	u32 instanceCount {};
	u32 firstInstance {};
	u32 stride {}; // NOTE: only here for gui, we don't forward it
	std::optional<i32> vertexOffset {};

	using CmdDerive::CmdDerive;

	std::string toString() const override;
	std::string_view nameDesc() const override { return "DrawMultiIndexed"; };
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct BindVertexBuffersCmd final : CmdDerive<Command, CommandType::bindVertexBuffers> {
	u32 firstBinding;
	span<BoundVertexBuffer> buffers;

	std::string toString() const override;
	void displayInspector(Gui&) const override;

	std::string_view nameDesc() const override { return "BindVertexBuffers"; }
	Category category() const override { return Category::bind; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct BindIndexBufferCmd final : CmdDerive<Command, CommandType::bindIndexBuffer> {
	Buffer* buffer {};
	VkDeviceSize offset {};
	VkIndexType indexType {};

	std::string_view nameDesc() const override { return "BindIndexBuffer"; }
	Category category() const override { return Category::bind; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct BindDescriptorSetCmd final : CmdDerive<Command, CommandType::bindDescriptorSet> {
	u32 firstSet;
	VkPipelineBindPoint pipeBindPoint;
	PipelineLayout* pipeLayout; // kept alive via shared_ptr in CommandBuffer
	span<DescriptorSet*> sets; // NOTE: handles may be invalid
	span<u32> dynamicOffsets;

	std::string toString() const override;
	std::string_view nameDesc() const override { return "BindDescriptorSets"; }
	Category category() const override { return Category::bind; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void displayInspector(Gui& gui) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct DispatchCmdBase : StateCmdBase {
	const ComputeState* state {};
	PushConstantData pushConstants;

	DispatchCmdBase() = default;
	DispatchCmdBase(CommandBuffer&);

	Category category() const override { return Category::dispatch; }
	void displayComputeState(Gui& gui) const;
	MatchVal doMatch(const DispatchCmdBase&) const;

	const DescriptorState& boundDescriptors() const override { return *state; }
	const Pipeline* boundPipe() const override { return state->pipe; }
	const PushConstantData& boundPushConstants() const override { return pushConstants; }
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct DispatchCmd final : CmdDerive<DispatchCmdBase, CommandType::dispatch> {
	u32 groupsX {};
	u32 groupsY {};
	u32 groupsZ {};

	using CmdDerive::CmdDerive;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "Dispatch"; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct DispatchIndirectCmd final : CmdDerive<DispatchCmdBase, CommandType::dispatchIndirect> {
	Buffer* buffer {};
	VkDeviceSize offset {};

	using CmdDerive::CmdDerive;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "DispatchIndirect"; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct DispatchBaseCmd final : CmdDerive<DispatchCmdBase, CommandType::dispatchBase> {
	u32 baseGroupX {};
	u32 baseGroupY {};
	u32 baseGroupZ {};
	u32 groupsX {};
	u32 groupsY {};
	u32 groupsZ {};

	using CmdDerive::CmdDerive;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "DispatchBase"; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// transfer commands
struct CopyImageCmd final : CmdDerive<Command, CommandType::copyImage> {
	Image* src {};
	Image* dst {};
	VkImageLayout srcLayout {};
	VkImageLayout dstLayout {};
	span<VkImageCopy2> copies;
	const void* pNext {};

	std::string toString() const override;
	Category category() const override { return Category::transfer; }
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "CopyImage"; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct CopyBufferToImageCmd final : CmdDerive<Command, CommandType::copyBufferToImage> {
	Buffer* src {};
	Image* dst {};
	VkImageLayout dstLayout {};
	span<VkBufferImageCopy2> copies;
	const void* pNext {};

	std::string toString() const override;
	Category category() const override { return Category::transfer; }
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "CopyBufferToImage"; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct CopyImageToBufferCmd final : CmdDerive<Command, CommandType::copyImageToBuffer> {
	Image* src {};
	Buffer* dst {};
	VkImageLayout srcLayout {};
	span<VkBufferImageCopy2> copies;
	const void* pNext {};

	std::string toString() const override;
	Category category() const override { return Category::transfer; }
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "CopyImageToBuffer"; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct BlitImageCmd final : CmdDerive<Command, CommandType::blitImage> {
	Image* src {};
	Image* dst {};
	VkImageLayout srcLayout {};
	VkImageLayout dstLayout {};
	span<VkImageBlit2> blits;
	VkFilter filter {};
	const void* pNext {};

	std::string toString() const override;
	Category category() const override { return Category::transfer; }
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "BlitImage"; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct ResolveImageCmd final : CmdDerive<Command, CommandType::resolveImage> {
	Image* src {};
	VkImageLayout srcLayout {};
	Image* dst {};
	VkImageLayout dstLayout {};
	span<VkImageResolve2> regions;
	const void* pNext {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "ResolveImage"; }
	Category category() const override { return Category::transfer; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct CopyBufferCmd final : CmdDerive<Command, CommandType::copyBuffer> {
	Buffer* src {};
	Buffer* dst {};
	span<VkBufferCopy2> regions;
	const void* pNext {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "CopyBuffer"; }
	Category category() const override { return Category::transfer; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct UpdateBufferCmd final : CmdDerive<Command, CommandType::updateBuffer> {
	Buffer* dst {};
	VkDeviceSize offset {};
	span<std::byte> data;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "UpdateBuffer"; }
	Category category() const override { return Category::transfer; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct FillBufferCmd final : CmdDerive<Command, CommandType::fillBuffer> {
	Buffer* dst {};
	VkDeviceSize offset {};
	VkDeviceSize size {};
	u32 data {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "FillBuffer"; }
	Category category() const override { return Category::transfer; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct ClearColorImageCmd final : CmdDerive<Command, CommandType::clearColorImage> {
	Image* dst {};
	VkClearColorValue color;
	VkImageLayout dstLayout {};
	span<VkImageSubresourceRange> ranges;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	Category category() const override { return Category::transfer; }
	std::string_view nameDesc() const override { return "ClearColorImage"; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct ClearDepthStencilImageCmd final : CmdDerive<Command, CommandType::clearDepthStencilImage> {
	Image* dst {};
	VkClearDepthStencilValue value {};
	VkImageLayout dstLayout {};
	span<VkImageSubresourceRange> ranges;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	Category category() const override { return Category::transfer; }
	std::string_view nameDesc() const override { return "ClearDepthStencilImage"; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct ClearAttachmentCmd final : CmdDerive<Command, CommandType::clearAttachment> {
	span<VkClearAttachment> attachments;
	span<VkClearRect> rects;

	std::string_view nameDesc() const override { return "ClearAttachment"; }
	void displayInspector(Gui& gui) const override;
	// NOTE: strictly speaking this isn't a transfer, we don't care.
	Category category() const override { return Category::transfer; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetEventCmd final : CmdDerive<Command, CommandType::setEvent> {
	Event* event {};
	VkPipelineStageFlags stageMask {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "SetEvent"; }
	Category category() const override { return Category::sync; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetEvent2Cmd final : CmdDerive<Barrier2CmdBase, CommandType::setEvent2> {
	Event* event {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "SetEvent"; }
	Category category() const override { return Category::sync; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct ResetEventCmd final : CmdDerive<Command, CommandType::resetEvent> {
	Event* event {};
	VkPipelineStageFlags2 stageMask {};
	bool legacy {}; // whether it's coming from a legacy (not sync2) command

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override {
		return legacy ? "ResetEvent" : "ResetEvent2";
	}
	Category category() const override { return Category::sync; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// Meta-command, inserted for each command buffer passed to CmdExecuteCommands.
// We want this since ExecuteCommandsCmd::children() would be awkward otherwise
// when there are multiple command buffers executed.
struct ExecuteCommandsChildCmd final : CmdDerive<ParentCommand, CommandType::executeCommandsChild> {
	CommandRecord* record_ {}; // kept alive in parent CommandRecord
	unsigned id_ {};

	std::string_view nameDesc() const override { return "ExecuteCommandsChild"; }
	std::string toString() const override;
	void record(const Device&, VkCommandBuffer, u32) const override {}
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }

	// Will return the commands from the embedded record.
	// This means that traversal of the command hierarchy can happen without
	// special consideration of ExecuteCommands, they will automatically
	// be part of the hierarchy, *without* being duplicated into it.
	Command* children() const override;
	const SectionStats& sectionStats() const override;
	ParentCommand* firstChildParent() const override;
};

struct ExecuteCommandsCmd final : CmdDerive<ParentCommand, CommandType::executeCommands> {
	ExecuteCommandsChildCmd* children_ {};
	SectionStats stats_ {};

	Command* children() const override { return children_; }
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "ExecuteCommands"; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
	ParentCommand* firstChildParent() const override { return children_; }
	const SectionStats& sectionStats() const override {
		// needed only for numChildSections, empty otherwise.
		return stats_;
	}
};

struct BeginDebugUtilsLabelCmd final : CmdDerive<SectionCommand, CommandType::beginDebugUtilsLabel> {
	const char* name {};
	std::array<float, 4> color; // NOTE: could use this in UI

	std::string toString() const override { return name; }
	void record(const Device&, VkCommandBuffer, u32) const override;

	// NOTE: yes, we return more than just the command here.
	// But that's because the command itself isn't of any use without the label.
	std::string_view nameDesc() const override { return name; }
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct EndDebugUtilsLabelCmd final : CmdDerive<Command, CommandType::endDebugUtilsLabel> {
	std::string_view nameDesc() const override { return "EndLabel"; }
	Category category() const override { return Category::end; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct InsertDebugUtilsLabelCmd final : CmdDerive<Command, CommandType::insertDebugUtilsLabel> {
	const char* name {};
	std::array<float, 4> color; // NOTE: could use this in UI

	std::string_view nameDesc() const override { return name; }
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
	void record(const Device&, VkCommandBuffer, u32) const override;
};

struct BindPipelineCmd final : CmdDerive<Command, CommandType::bindPipeline> {
	VkPipelineBindPoint bindPoint {};
	Pipeline* pipe {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "BindPipeline"; }
	Category category() const override { return Category::bind; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct PushConstantsCmd final : CmdDerive<Command, CommandType::pushConstants> {
	PipelineLayout* pipeLayout; // kept alive via shared_ptr in CommandBuffer
	VkShaderStageFlags stages {};
	u32 offset {};
	span<std::byte> values;

	std::string_view nameDesc() const override { return "PushConstants"; }
	Category category() const override { return Category::bind; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// dynamic state
struct SetViewportCmd final : CmdDerive<Command, CommandType::setViewport> {
	u32 first {};
	span<VkViewport> viewports;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetViewport"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void displayInspector(Gui& gui) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetScissorCmd final : CmdDerive<Command, CommandType::setScissor> {
	u32 first {};
	span<VkRect2D> scissors;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetScissor"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void displayInspector(Gui& gui) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetLineWidthCmd final : CmdDerive<Command, CommandType::setLineWidth> {
	float width {};

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetLineWidth"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void displayInspector(Gui& gui) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetDepthBiasCmd final : CmdDerive<Command, CommandType::setDepthBias> {
	DynamicStateDepthBias state {};

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetDepthBias"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetDepthBoundsCmd final : CmdDerive<Command, CommandType::setDepthBounds> {
	float min {};
	float max {};

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetDepthBounds"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetBlendConstantsCmd final : CmdDerive<Command, CommandType::setBlendConstants> {
	std::array<float, 4> values {};

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetBlendConstants"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetStencilCompareMaskCmd final : CmdDerive<Command, CommandType::setStencilCompareMask> {
	VkStencilFaceFlags faceMask {};
	u32 value {};

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetStencilCompareMask"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetStencilWriteMaskCmd final : CmdDerive<Command, CommandType::setStencilWriteMask> {
	VkStencilFaceFlags faceMask {};
	u32 value {};

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetStencilWriteMask"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetStencilReferenceCmd final : CmdDerive<Command, CommandType::setStencilReference> {
	VkStencilFaceFlags faceMask {};
	u32 value {};

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetStencilReference"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// query pool
struct BeginQueryCmd final : CmdDerive<Command, CommandType::beginQuery> {
	QueryPool* pool {};
	u32 query {};
	VkQueryControlFlags flags {};

	Category category() const override { return Category::query; }
	std::string_view nameDesc() const override { return "BeginQuery"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct EndQueryCmd final : CmdDerive<Command, CommandType::endQuery> {
	QueryPool* pool {};
	u32 query {};

	Category category() const override { return Category::query; }
	std::string_view nameDesc() const override { return "EndQuery"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct ResetQueryPoolCmd final : CmdDerive<Command, CommandType::resetQueryPool> {
	QueryPool* pool {};
	u32 first {};
	u32 count {};

	Category category() const override { return Category::query; }
	std::string_view nameDesc() const override { return "ResetQueryPool"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct WriteTimestampCmd final : CmdDerive<Command, CommandType::writeTimestamp> {
	QueryPool* pool {};
	VkPipelineStageFlagBits2 stage {};
	u32 query {};
	bool legacy {}; // whether it's coming from a legacy (not sync2) command

	Category category() const override { return Category::query; }
	std::string_view nameDesc() const override {
		return legacy ? "WriteTimestamp" : "WriteTimestamp2";
	}
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct CopyQueryPoolResultsCmd final : CmdDerive<Command, CommandType::copyQueryPoolResults> {
	QueryPool* pool {};
	u32 first {};
	u32 count {};
	Buffer* dstBuffer {};
	VkDeviceSize dstOffset {};
	VkDeviceSize stride {};
	VkQueryResultFlags flags {};

	Category category() const override { return Category::query; }
	std::string_view nameDesc() const override { return "CopyQueryPoolResults"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// Push descriptor commands
struct PushDescriptorSetCmd final : CmdDerive<Command, CommandType::pushDescriptorSet> {
	VkPipelineBindPoint bindPoint {};
	PipelineLayout* pipeLayout {};
	u32 set {};

	// The individual pImageInfo, pBufferInfo, pTexelBufferView arrays
	// are allocated in the CommandRecord-owned memory as well.
	span<VkWriteDescriptorSet> descriptorWrites;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "PushDescriptorSet"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct PushDescriptorSetWithTemplateCmd final : CmdDerive<Command, CommandType::pushDescriptorSetWithTemplate> {
	DescriptorUpdateTemplate* updateTemplate {};
	PipelineLayout* pipeLayout {};
	u32 set {};
	span<const std::byte> data;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "PushDescriptorSetWithTemplate"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// VK_KHR_fragment_shading_rate
struct SetFragmentShadingRateCmd final : CmdDerive<Command, CommandType::setFragmentShadingRate> {
	VkExtent2D fragmentSize;
	std::array<VkFragmentShadingRateCombinerOpKHR, 2> combinerOps;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetFragmentShadingRateCmd"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// VK_EXT_conditional_rendering
struct BeginConditionalRenderingCmd final : CmdDerive<SectionCommand, CommandType::beginConditionalRendering> {
	Buffer* buffer;
	VkDeviceSize offset;
	VkConditionalRenderingFlagsEXT flags;

	Category category() const override { return Category::other; }
	std::string_view nameDesc() const override { return "BeginConditionalRendering"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct EndConditionalRenderingCmd final : CmdDerive<Command, CommandType::endConditionalRendering> {
	Category category() const override { return Category::end; }
	std::string_view nameDesc() const override { return "EndConditionalRendering"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// VK_EXT_line_rasterization
struct SetLineStippleCmd final : CmdDerive<Command, CommandType::setLineStipple> {
	u32 stippleFactor;
	u16 stipplePattern;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetLineStipple"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// VK_EXT_extended_dynamic_state
struct SetCullModeCmd final : CmdDerive<Command, CommandType::setCullMode> {
	VkCullModeFlags cullMode;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetCullMode"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetFrontFaceCmd final : CmdDerive<Command, CommandType::setFrontFace> {
	VkFrontFace frontFace;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetFrontFace"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetPrimitiveTopologyCmd final : CmdDerive<Command, CommandType::setPrimitiveTopology> {
	VkPrimitiveTopology topology;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetPrimitiveTopology"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetViewportWithCountCmd final : CmdDerive<Command, CommandType::setViewportWithCount> {
	span<VkViewport> viewports;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetViewportWithCount"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetScissorWithCountCmd final : CmdDerive<Command, CommandType::setScissorWithCount> {
	span<VkRect2D> scissors;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetScissorWithCount"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// BindVertexBuffers2Cmd should be mapped via BindVertexBuffers

struct SetDepthTestEnableCmd final : CmdDerive<Command, CommandType::setDepthTestEnable> {
	bool enable;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetDepthTestEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetDepthWriteEnableCmd final : CmdDerive<Command, CommandType::setDepthWriteEnable> {
	bool enable;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetDepthWriteEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetDepthCompareOpCmd final : CmdDerive<Command, CommandType::setDepthCompareOp> {
	VkCompareOp op;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetDepthCompareOp"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetDepthBoundsTestEnableCmd final : CmdDerive<Command, CommandType::setDepthBoundsTestEnable> {
	bool enable;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetDepthBoundsTestEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetStencilTestEnableCmd final : CmdDerive<Command, CommandType::setStencilTestEnable> {
	bool enable;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetStencilTestEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetStencilOpCmd final : CmdDerive<Command, CommandType::setStencilOp> {
    VkStencilFaceFlags faceMask;
    VkStencilOp failOp;
    VkStencilOp passOp;
    VkStencilOp depthFailOp;
    VkCompareOp compareOp;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetStencilOp"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// VK_EXT_extended_dynamic_state2
struct SetPatchControlPointsCmd final : CmdDerive<Command, CommandType::setPatchControlPoints> {
	u32 patchControlPoints;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetPatchControlPoints"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetRasterizerDiscardEnableCmd final : CmdDerive<Command, CommandType::setRasterizerDiscardEnable> {
	bool enable;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetRasterizerDiscardEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetDepthBiasEnableCmd final : CmdDerive<Command, CommandType::setDepthBias> {
	bool enable;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetDepthBiasEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetLogicOpCmd final : CmdDerive<Command, CommandType::setLogicOp> {
	VkLogicOp logicOp;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetLogicOp"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetPrimitiveRestartEnableCmd final : CmdDerive<Command, CommandType::setPrimitiveRestartEnable> {
	bool enable;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetPrimitiveRestartEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// VK_EXT_sample_locations
struct SetSampleLocationsCmd final : CmdDerive<Command, CommandType::setSampleLocations> {
	VkSampleLocationsInfoEXT info;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetSampleLocations"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// VK_EXT_discard_rectangles
struct SetDiscardRectangleCmd final : CmdDerive<Command, CommandType::setDisacrdRectangle> {
	u32 first;
	span<VkRect2D> rects;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetDiscardRectangle"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// VK_KHR_acceleration_structure
struct CopyAccelStructCmd final : CmdDerive<Command, CommandType::copyAccelStruct> {
	const void* pNext {};
	AccelStruct* src {};
	AccelStruct* dst {};
    VkCopyAccelerationStructureModeKHR mode;

	Category category() const override { return Category::transfer; }
	std::string_view nameDesc() const override { return "CopyAccelerationStructure"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct CopyAccelStructToMemoryCmd final : CmdDerive<Command, CommandType::copyAccelStructToMemory> {
	const void* pNext {};
	AccelStruct* src {};
    VkDeviceOrHostAddressKHR dst {};
    VkCopyAccelerationStructureModeKHR mode;

	Category category() const override { return Category::transfer; }
	std::string_view nameDesc() const override { return "CopyAccelerationStructureToMemory"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct CopyMemoryToAccelStructCmd final : CmdDerive<Command, CommandType::copyMemoryToAccelStruct> {
	const void* pNext {};
    VkDeviceOrHostAddressConstKHR src {};
	AccelStruct* dst {};
    VkCopyAccelerationStructureModeKHR mode;

	Category category() const override { return Category::transfer; }
	std::string_view nameDesc() const override { return "CopyMemoryToAccelerationStructure"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct WriteAccelStructsPropertiesCmd final : CmdDerive<Command, CommandType::writeAccelStructsProperties> {
	span<AccelStruct*> accelStructs;
	VkQueryType queryType {};
	QueryPool* queryPool {};
	u32 firstQuery {};

	Category category() const override { return Category::query; }
	std::string_view nameDesc() const override { return "WriteAccelerationStructuresProperties"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct BuildAccelStructsCmd final : CmdDerive<Command, CommandType::buildAccelStructs> {
	span<VkAccelerationStructureBuildGeometryInfoKHR> buildInfos; // already handle-fwd-patched
	span<span<VkAccelerationStructureBuildRangeInfoKHR>> buildRangeInfos;
	span<AccelStruct*> srcs;
	span<AccelStruct*> dsts;

	Category category() const override { return Category::buildAccelStruct; }
	std::string_view nameDesc() const override { return "BuildAccelerationStructures"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct BuildAccelStructsIndirectCmd final : CmdDerive<Command, CommandType::buildAccelStructsIndirect> {
	span<VkAccelerationStructureBuildGeometryInfoKHR> buildInfos; // already handle-fwd-patched
	span<AccelStruct*> srcs;
	span<AccelStruct*> dsts;

	// indirect info. All span have a size equal to buildInfos.size()
	span<VkDeviceAddress> indirectAddresses;
	span<u32> indirectStrides;
	span<u32*> maxPrimitiveCounts;

	Category category() const override { return Category::buildAccelStruct; }
	std::string_view nameDesc() const override { return "BuildAccelerationStructuresIndirect"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// VK_KHR_ray_tracing_pipeline
struct TraceRaysCmdBase : StateCmdBase {
	const RayTracingState* state {};
	PushConstantData pushConstants;

	TraceRaysCmdBase() = default;
	TraceRaysCmdBase(CommandBuffer& cb);

	Category category() const override { return Category::traceRays; }
	MatchVal doMatch(const TraceRaysCmdBase& cmd) const;

	const DescriptorState& boundDescriptors() const override { return *state; }
	const Pipeline* boundPipe() const override { return state->pipe; }
	const PushConstantData& boundPushConstants() const override { return pushConstants; }
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct TraceRaysCmd final : CmdDerive<TraceRaysCmdBase, CommandType::traceRays> {
	u32 width;
	u32 height;
	u32 depth;

	VkStridedDeviceAddressRegionKHR raygenBindingTable;
	VkStridedDeviceAddressRegionKHR missBindingTable;
	VkStridedDeviceAddressRegionKHR hitBindingTable;
	VkStridedDeviceAddressRegionKHR callableBindingTable;

	using CmdDerive::CmdDerive;
	std::string_view nameDesc() const override { return "TraceRays"; }
	std::string toString() const override;
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct TraceRaysIndirectCmd final : CmdDerive<TraceRaysCmdBase, CommandType::traceRaysIndirect> {
	VkDeviceAddress indirectDeviceAddress;
	VkStridedDeviceAddressRegionKHR raygenBindingTable;
	VkStridedDeviceAddressRegionKHR missBindingTable;
	VkStridedDeviceAddressRegionKHR hitBindingTable;
	VkStridedDeviceAddressRegionKHR callableBindingTable;

	using CmdDerive::CmdDerive;
	std::string_view nameDesc() const override { return "TraceRaysIndirect"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

struct SetRayTracingPipelineStackSizeCmd final : CmdDerive<Command, CommandType::setRayTracingPipelineStackSize> {
	u32 stackSize;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetRayTracingPipelineStackSize"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// VK_KHR_dynamic_rendering
struct BeginRenderingCmd final : CmdDerive<RenderSectionCommand, CommandType::beginRendering> {
	struct Attachment {
		ImageView* view {};
		VkImageLayout imageLayout;
		VkResolveModeFlagBits resolveMode;
		ImageView* resolveView {};
		VkImageLayout resolveImageLayout;
		VkAttachmentLoadOp loadOp;
		VkAttachmentStoreOp storeOp;
		VkClearValue clearValue;
	};

	u32 layerCount {};
	u32 viewMask {};
	VkRenderingFlags flags {};
	span<Attachment> colorAttachments;
	Attachment depthAttachment; // only valid if view != null
	Attachment stencilAttachment; // only valid if view != null
	VkRect2D renderArea;

	void record(const Device&, VkCommandBuffer cb,
		bool skipResolves,
		std::optional<VkAttachmentLoadOp> overrideLoad,
		std::optional<VkAttachmentStoreOp> overrideStore) const;

	const Attachment* findAttachment(const Image& img) const;

	std::string_view nameDesc() const override { return "BeginRendering"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }

	// TODO: inspector
	// TODO: toString?
};

struct EndRenderingCmd final : CmdDerive<SectionCommand, CommandType::endRendering> {
	std::string_view nameDesc() const override { return "EndRendering"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
};

// VK_EXT_vertex_input_dynamic_state
struct SetVertexInputCmd final : CmdDerive<Command, CommandType::setVertexInput> {
	span<VkVertexInputBindingDescription2EXT> bindings;
	span<VkVertexInputAttributeDescription2EXT> attribs;

	std::string_view nameDesc() const override { return "SetVertexInput"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
	Category category() const override { return Category::bind; }
};

// VK_EXT_color_write_enable
struct SetColorWriteEnableCmd final : CmdDerive<Command, CommandType::setColorWriteEnable> {
	span<VkBool32> writeEnables;

	std::string_view nameDesc() const override { return "SetColorWriteEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void visit(CommandVisitor& v) const override { doVisit(v, *this); }
	Category category() const override { return Category::bind; }
};

// Visitor
// Might seem overkill to have this here but it's useful in multiple
// scenarios: in many cases we have extensive external functionality operating
// on commands that shouldn't be part of the commands themselves from a design
// POV. E.g. advanced gui functionality or serialization; implementing them
// freely makes for a cleaner, more modular design.
struct CommandVisitor {
	virtual ~CommandVisitor() = default;

	virtual void visit(const Command&) = 0;
	virtual void visit(const ParentCommand& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SectionCommand& cmd) { visit(static_cast<const ParentCommand&>(cmd)); }
	virtual void visit(const RenderSectionCommand& cmd) { visit(static_cast<const SectionCommand&>(cmd)); }
	virtual void visit(const RootCommand& cmd) { visit(static_cast<const SectionCommand&>(cmd)); }

	virtual void visit(const BeginRenderPassCmd& cmd) { visit(static_cast<const SectionCommand&>(cmd)); }
	virtual void visit(const EndRenderPassCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SubpassCmd& cmd) { visit(static_cast<const SectionCommand&>(cmd)); }
	virtual void visit(const FirstSubpassCmd& cmd) { visit(static_cast<const SubpassCmd&>(cmd)); }
	virtual void visit(const NextSubpassCmd& cmd) { visit(static_cast<const SubpassCmd&>(cmd)); }

	virtual void visit(const BeginRenderingCmd& cmd) { visit(static_cast<const RenderSectionCommand&>(cmd)); }
	virtual void visit(const EndRenderingCmd& cmd) { visit(static_cast<const Command&>(cmd)); }

	virtual void visit(const BeginDebugUtilsLabelCmd& cmd) { visit(static_cast<const SectionCommand&>(cmd)); }
	virtual void visit(const EndDebugUtilsLabelCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const InsertDebugUtilsLabelCmd& cmd) { visit(static_cast<const Command&>(cmd)); }

	virtual void visit(const StateCmdBase& cmd) { visit(static_cast<const Command&>(cmd)); }

	virtual void visit(const DrawCmdBase& cmd) { visit(static_cast<const StateCmdBase&>(cmd)); }
	virtual void visit(const DrawCmd& cmd) { visit(static_cast<const DrawCmdBase&>(cmd)); }
	virtual void visit(const DrawIndexedCmd& cmd) { visit(static_cast<const DrawCmdBase&>(cmd)); }
	virtual void visit(const DrawIndirectCmd& cmd) { visit(static_cast<const DrawCmdBase&>(cmd)); }
	virtual void visit(const DrawIndirectCountCmd& cmd) { visit(static_cast<const DrawCmdBase&>(cmd)); }
	virtual void visit(const DrawMultiCmd& cmd) { visit(static_cast<const DrawCmdBase&>(cmd)); }
	virtual void visit(const DrawMultiIndexedCmd& cmd) { visit(static_cast<const DrawCmdBase&>(cmd)); }

	virtual void visit(const DispatchCmdBase& cmd) { visit(static_cast<const StateCmdBase&>(cmd)); }
	virtual void visit(const DispatchCmd& cmd) { visit(static_cast<const DispatchCmdBase&>(cmd)); }
	virtual void visit(const DispatchIndirectCmd& cmd) { visit(static_cast<const DispatchCmdBase&>(cmd)); }
	virtual void visit(const DispatchBaseCmd& cmd) { visit(static_cast<const DispatchCmdBase&>(cmd)); }

	virtual void visit(const CopyImageCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const CopyBufferToImageCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const CopyImageToBufferCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const BlitImageCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const ResolveImageCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const CopyBufferCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const UpdateBufferCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const FillBufferCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const ClearColorImageCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const ClearDepthStencilImageCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const ClearAttachmentCmd& cmd) { visit(static_cast<const Command&>(cmd)); }

	virtual void visit(const SetEventCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetEvent2Cmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const ResetEventCmd& cmd) { visit(static_cast<const Command&>(cmd)); }

	virtual void visit(const ExecuteCommandsChildCmd& cmd) { visit(static_cast<const ParentCommand&>(cmd)); }
	virtual void visit(const ExecuteCommandsCmd& cmd) { visit(static_cast<const ParentCommand&>(cmd)); }

	virtual void visit(const TraceRaysCmdBase& cmd) { visit(static_cast<const StateCmdBase&>(cmd)); }
	virtual void visit(const TraceRaysCmd& cmd) { visit(static_cast<const TraceRaysCmdBase&>(cmd)); }
	virtual void visit(const TraceRaysIndirectCmd& cmd) { visit(static_cast<const TraceRaysCmdBase&>(cmd)); }

	virtual void visit(const BeginConditionalRenderingCmd& cmd) { visit(static_cast<const SectionCommand&>(cmd)); }
	virtual void visit(const EndConditionalRenderingCmd& cmd) { visit(static_cast<const Command&>(cmd)); }

	virtual void visit(const BarrierCmdBase& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const Barrier2CmdBase& cmd) { visit(static_cast<const Command&>(cmd)); }

	virtual void visit(const WaitEventsCmd& cmd) { visit(static_cast<const BarrierCmdBase&>(cmd)); }
	virtual void visit(const BarrierCmd& cmd) { visit(static_cast<const BarrierCmdBase&>(cmd)); }
	virtual void visit(const WaitEvents2Cmd& cmd) { visit(static_cast<const Barrier2CmdBase&>(cmd)); }
	virtual void visit(const Barrier2Cmd& cmd) { visit(static_cast<const Barrier2CmdBase&>(cmd)); }

	virtual void visit(const BindVertexBuffersCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const BindIndexBufferCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const BindPipelineCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const BindDescriptorSetCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetScissorCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetViewportCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetLineWidthCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetDepthBiasCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetDepthBoundsCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetBlendConstantsCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetStencilCompareMaskCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetStencilWriteMaskCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetStencilReferenceCmd& cmd) { visit(static_cast<const Command&>(cmd)); }

	virtual void visit(const BeginQueryCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const EndQueryCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const ResetQueryPoolCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const WriteTimestampCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const CopyQueryPoolResultsCmd& cmd) { visit(static_cast<const Command&>(cmd)); }

	virtual void visit(const PushConstantsCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const PushDescriptorSetCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const PushDescriptorSetWithTemplateCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetFragmentShadingRateCmd& cmd) { visit(static_cast<const Command&>(cmd)); }

	virtual void visit(const SetLineStippleCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetCullModeCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetFrontFaceCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetPrimitiveTopologyCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetScissorWithCountCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetViewportWithCountCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetDepthTestEnableCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetDepthWriteEnableCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetDepthCompareOpCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetDepthBoundsTestEnableCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetStencilTestEnableCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetStencilOpCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetPatchControlPointsCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetRasterizerDiscardEnableCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetDepthBiasEnableCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetLogicOpCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetPrimitiveRestartEnableCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetSampleLocationsCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetDiscardRectangleCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetVertexInputCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetColorWriteEnableCmd& cmd) { visit(static_cast<const Command&>(cmd)); }

	virtual void visit(const CopyAccelStructCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const CopyAccelStructToMemoryCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const CopyMemoryToAccelStructCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const WriteAccelStructsPropertiesCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const BuildAccelStructsCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const BuildAccelStructsIndirectCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
	virtual void visit(const SetRayTracingPipelineStackSizeCmd& cmd) { visit(static_cast<const Command&>(cmd)); }
};

template<typename F>
struct TemplateCommandVisitor : CommandVisitor {
	F f;

	TemplateCommandVisitor(F xf) : f(std::move(xf)) {}

	void visit(const Command& cmd) override { f(cmd); }
	void visit(const ParentCommand& cmd) override { f(cmd); }
	void visit(const SectionCommand& cmd) override { f(cmd); }
	void visit(const RenderSectionCommand& cmd) override { f(cmd); }
	void visit(const RootCommand& cmd) override { f(cmd); }

	void visit(const BeginRenderPassCmd& cmd) override { f(cmd); }
	void visit(const EndRenderPassCmd& cmd) override { f(cmd); }
	void visit(const SubpassCmd& cmd) override { f(cmd); }
	void visit(const FirstSubpassCmd& cmd) override { f(cmd); }
	void visit(const NextSubpassCmd& cmd) override { f(cmd); }

	void visit(const BeginRenderingCmd& cmd) override { f(cmd); }
	void visit(const EndRenderingCmd& cmd) override { f(cmd); }

	void visit(const BeginDebugUtilsLabelCmd& cmd) override { f(cmd); }
	void visit(const EndDebugUtilsLabelCmd& cmd) override { f(cmd); }
	void visit(const InsertDebugUtilsLabelCmd& cmd) override { f(cmd); }

	void visit(const StateCmdBase& cmd) override { f(cmd); }
	void visit(const DrawCmdBase& cmd) override { f(cmd); }
	void visit(const DrawCmd& cmd) override { f(cmd); }
	void visit(const DrawIndexedCmd& cmd) override { f(cmd); }
	void visit(const DrawIndirectCmd& cmd) override { f(cmd); }
	void visit(const DrawIndirectCountCmd& cmd) override { f(cmd); }
	void visit(const DrawMultiCmd& cmd) override { f(cmd); }
	void visit(const DrawMultiIndexedCmd& cmd) override { f(cmd); }

	void visit(const DispatchCmdBase& cmd) override { f(cmd); }
	void visit(const DispatchCmd& cmd) override { f(cmd); }
	void visit(const DispatchIndirectCmd& cmd) override { f(cmd); }
	void visit(const DispatchBaseCmd& cmd) override { f(cmd); }

	void visit(const CopyImageCmd& cmd) override { f(cmd); }
	void visit(const CopyBufferToImageCmd& cmd) override { f(cmd); }
	void visit(const CopyImageToBufferCmd& cmd) override { f(cmd); }
	void visit(const BlitImageCmd& cmd) override { f(cmd); }
	void visit(const ResolveImageCmd& cmd) override { f(cmd); }
	void visit(const CopyBufferCmd& cmd) override { f(cmd); }
	void visit(const UpdateBufferCmd& cmd) override { f(cmd); }
	void visit(const FillBufferCmd& cmd) override { f(cmd); }
	void visit(const ClearColorImageCmd& cmd) override { f(cmd); }
	void visit(const ClearDepthStencilImageCmd& cmd) override { f(cmd); }
	void visit(const ClearAttachmentCmd& cmd) override { f(cmd); }

	void visit(const SetEventCmd& cmd) override { f(cmd); }
	void visit(const SetEvent2Cmd& cmd) override { f(cmd); }
	void visit(const ResetEventCmd& cmd) override { f(cmd); }

	void visit(const ExecuteCommandsChildCmd& cmd) override { f(cmd); }
	void visit(const ExecuteCommandsCmd& cmd) override { f(cmd); }

	void visit(const TraceRaysCmdBase& cmd) override { f(cmd); }
	void visit(const TraceRaysCmd& cmd) override { f(cmd); }
	void visit(const TraceRaysIndirectCmd& cmd) override { f(cmd); }

	void visit(const BeginConditionalRenderingCmd& cmd) override { f(cmd); }
	void visit(const EndConditionalRenderingCmd& cmd) override { f(cmd); }

	void visit(const BarrierCmdBase& cmd) override { f(cmd); }
	void visit(const Barrier2CmdBase& cmd) override { f(cmd); }

	void visit(const WaitEventsCmd& cmd) override { f(cmd); }
	void visit(const BarrierCmd& cmd) override { f(cmd); }
	void visit(const WaitEvents2Cmd& cmd) override { f(cmd); }
	void visit(const Barrier2Cmd& cmd) override { f(cmd); }

	void visit(const BindVertexBuffersCmd& cmd) override { f(cmd); }
	void visit(const BindIndexBufferCmd& cmd) override { f(cmd); }
	void visit(const BindPipelineCmd& cmd) override { f(cmd); }
	void visit(const BindDescriptorSetCmd& cmd) override { f(cmd); }

	void visit(const SetLineWidthCmd& cmd) override { f(cmd); }
	void visit(const SetDepthBiasCmd& cmd) override { f(cmd); }
	void visit(const SetDepthBoundsCmd& cmd) override { f(cmd); }
	void visit(const SetBlendConstantsCmd& cmd) override { f(cmd); }
	void visit(const SetStencilCompareMaskCmd& cmd) override { f(cmd); }
	void visit(const SetStencilWriteMaskCmd& cmd) override { f(cmd); }
	void visit(const SetStencilReferenceCmd& cmd) override { f(cmd); }

	void visit(const BeginQueryCmd& cmd) override { f(cmd); }
	void visit(const EndQueryCmd& cmd) override { f(cmd); }
	void visit(const ResetQueryPoolCmd& cmd) override { f(cmd); }
	void visit(const WriteTimestampCmd& cmd) override { f(cmd); }
	void visit(const CopyQueryPoolResultsCmd& cmd) override { f(cmd); }

	void visit(const PushConstantsCmd& cmd) override { f(cmd); }
	void visit(const PushDescriptorSetCmd& cmd) override { f(cmd); }
	void visit(const PushDescriptorSetWithTemplateCmd& cmd) override { f(cmd); }
	void visit(const SetFragmentShadingRateCmd& cmd) override { f(cmd); }

	void visit(const SetLineStippleCmd& cmd) override { f(cmd); }
	void visit(const SetCullModeCmd& cmd) override { f(cmd); }
	void visit(const SetFrontFaceCmd& cmd) override { f(cmd); }
	void visit(const SetPrimitiveTopologyCmd& cmd) override { f(cmd); }
	void visit(const SetScissorWithCountCmd& cmd) override { f(cmd); }
	void visit(const SetViewportWithCountCmd& cmd) override { f(cmd); }
	void visit(const SetDepthTestEnableCmd& cmd) override { f(cmd); }
	void visit(const SetDepthWriteEnableCmd& cmd) override { f(cmd); }
	void visit(const SetDepthCompareOpCmd& cmd) override { f(cmd); }
	void visit(const SetDepthBoundsTestEnableCmd& cmd) override { f(cmd); }
	void visit(const SetStencilTestEnableCmd& cmd) override { f(cmd); }
	void visit(const SetStencilOpCmd& cmd) override { f(cmd); }
	void visit(const SetPatchControlPointsCmd& cmd) override { f(cmd); }
	void visit(const SetRasterizerDiscardEnableCmd& cmd) override { f(cmd); }
	void visit(const SetDepthBiasEnableCmd& cmd) override { f(cmd); }
	void visit(const SetLogicOpCmd& cmd) override { f(cmd); }
	void visit(const SetPrimitiveRestartEnableCmd& cmd) override { f(cmd); }
	void visit(const SetSampleLocationsCmd& cmd) override { f(cmd); }
	void visit(const SetDiscardRectangleCmd& cmd) override { f(cmd); }
	void visit(const SetVertexInputCmd& cmd) override { f(cmd); }
	void visit(const SetColorWriteEnableCmd& cmd) override { f(cmd); }

	void visit(const CopyAccelStructCmd& cmd) override { f(cmd); }
	void visit(const CopyAccelStructToMemoryCmd& cmd) override { f(cmd); }
	void visit(const CopyMemoryToAccelStructCmd& cmd) override { f(cmd); }
	void visit(const WriteAccelStructsPropertiesCmd& cmd) override { f(cmd); }
	void visit(const BuildAccelStructsCmd& cmd) override { f(cmd); }
	void visit(const BuildAccelStructsIndirectCmd& cmd) override { f(cmd); }
	void visit(const SetRayTracingPipelineStackSizeCmd& cmd) override { f(cmd); }
};

template<typename C>
void doVisit(CommandVisitor& v, C& cmd) {
	templatize<C>(v).visit(cmd);
}

// Return true for indirect commands (i.e. state or transfer commands that
// contain command information in a passed address/buffer)
bool isIndirect(const Command&);

// Should be a lot faster than dynamic_cast on most compilers.
// Designed to be a drop-in replacmenet for dynamic_cast.
template<typename DstCmd, typename SrcCmd>
std::enable_if_t<std::is_pointer_v<DstCmd>, DstCmd> commandCast(SrcCmd* cmd) {
	using D = std::remove_pointer_t<DstCmd>;
	if(cmd && cmd->type() == D::staticType()) {
		return static_cast<DstCmd>(cmd);
	}

	return nullptr;
}

template<typename DstCmd, typename SrcCmd>
std::enable_if_t<std::is_pointer_v<DstCmd>, DstCmd> commandCast(SrcCmd& cmd) {
	using D = std::remove_pointer_t<DstCmd>;
	if(cmd.type() == D::staticType()) {
		return static_cast<DstCmd>(&cmd);
	}

	return nullptr;
}

template<typename DstCmd, typename SrcCmd>
std::enable_if_t<std::is_reference_v<DstCmd>, DstCmd> commandCast(SrcCmd& cmd) {
	using D = std::remove_reference_t<DstCmd>;
	if(cmd.type() == D::staticType()) {
		return static_cast<DstCmd>(cmd);
	}

	throw std::bad_cast();
}


} // namespace vil

#ifdef __GNUC__
	#pragma GCC diagnostic pop
#endif // __GNUC__
