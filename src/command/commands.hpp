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
	bindShaders,
	setDepthClampRange,

	// VK_EXT_mesh_shader
	drawMeshTasks,
	drawMeshTasksIndirect,
	drawMeshTasksIndirectCount,

	// VK_EXT_device_generated_commands
	executeGeneratedCommands,
	preprocessGeneratedCommands,

	// VK_EXT_extended_dynamic_state3
	setDepthClampEnable,
	setPolygonMode,
	setRasterizationSamples,
	setSampleMask,
	setAlphaToCoverageEnable,
	setAlphaToOneEnable,
	setLogicOpEnable,
	setColorBlendEnable,
	setColorBlendEquation,
	setColorWriteMask,
	setTessellationDomainOrigin,
	setRasterizationStream,
	setConservativeRasterizationMode,
	setExtraPrimitiveOverestimationSize,
	setDepthClipEnable,
	setSampleLocationsEnable,
	setColorBlendAdvanced,
	setProvokingVertexMode,
	setLineRasterizationMode,
	setLineStippleEnable,
	setDepthClipNegativeOneToOneEXT,

	// VK_EXT_depth_bias_control
	setDepthBias2,

	count,
};

using CommandCategoryFlags = nytl::Flags<CommandCategory>;

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

	const void* pNext {};

	void displayInspector(Gui& gui) const override;
	MatchVal doMatch(const Barrier2CmdBase& rhs) const;

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
};

struct WaitEvents2Cmd : CmdDerive<Barrier2CmdBase, CommandType::waitEvents2> {
	span<Event*> events;

	std::string_view nameDesc() const override { return "WaitEvents2"; }
	Category category() const override { return Category::sync; }
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer, u32) const override;
};

struct BarrierCmd : CmdDerive<BarrierCmdBase, CommandType::barrier> {
    VkDependencyFlags dependencyFlags {};

	std::string_view nameDesc() const override { return "PipelineBarrier"; }
	Category category() const override { return Category::sync; }
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer, u32) const override;
};

struct Barrier2Cmd : CmdDerive<Barrier2CmdBase, CommandType::barrier2> {
	std::string_view nameDesc() const override { return "PipelineBarrier2"; }
	Category category() const override { return Category::sync; }
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer, u32) const override;
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
};

// Meta command needed for correct hierachy. We want each subpass to
// have its own section.
struct FirstSubpassCmd final : CmdDerive<SubpassCmd, CommandType::firstSubpass> {
	using CmdDerive::CmdDerive;
	std::string_view nameDesc() const override { return "Subpass 0"; }
	void record(const Device&, VkCommandBuffer, u32) const override {}
};

struct EndRenderPassCmd final : CmdDerive<Command, CommandType::endRenderPass> {
	VkSubpassEndInfo endInfo {}; // for the previous subpass

	std::string_view nameDesc() const override { return "EndRenderPass"; }
	Category category() const override { return Category::end; }
	void record(const Device&, VkCommandBuffer, u32) const override;
};

// Base command from which all draw, dispatch and traceRays command are derived.
struct StateCmdBase : Command {
	virtual const DescriptorState& boundDescriptors() const = 0;
	virtual Pipeline* boundPipe() const = 0;
	virtual const PushConstantData& boundPushConstants() const = 0;
};

inline bool isStateCmd(const Command& cmd) {
	return cmd.category() == CommandCategory::dispatch ||
		cmd.category() == CommandCategory::draw ||
		cmd.category() == CommandCategory::traceRays;
}

struct DrawCmdBase : StateCmdBase {
	const GraphicsState* state {};
	PushConstantData pushConstants;

	DrawCmdBase(); // dummy constructor
	DrawCmdBase(CommandBuffer& cb);

	Category category() const override { return Category::draw; }
	void displayGrahpicsState(Gui& gui, bool indices) const;
	MatchVal doMatch(const DrawCmdBase& cmd, bool indexed) const;

	const DescriptorState& boundDescriptors() const override { return *state; }
	GraphicsPipeline* boundPipe() const override { return state->pipe; }

	virtual bool isIndexed() const = 0;

	const PushConstantData& boundPushConstants() const override { return pushConstants; }
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
	bool isIndexed() const override { return false; }
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
	bool isIndexed() const override { return indexed; }
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
	bool isIndexed() const override { return true; }
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
	bool isIndexed() const override { return indexed; }
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
	bool isIndexed() const override { return false; }
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
	bool isIndexed() const override { return true; }
};

struct DrawMeshTasksCmd final : CmdDerive<DrawCmdBase, CommandType::drawMeshTasks> {
	u32 groupCountX {};
	u32 groupCountY {};
	u32 groupCountZ {};

	using CmdDerive::CmdDerive;
	std::string_view nameDesc() const override { return "DrawMeshTasks"; };
	void record(const Device&, VkCommandBuffer, u32) const override;
	bool isIndexed() const override { return false; }
};

struct DrawMeshTasksIndirectCmd final : CmdDerive<DrawCmdBase, CommandType::drawMeshTasksIndirect> {
	Buffer* buffer {};
	VkDeviceSize offset {};
	u32 drawCount {};
	u32 stride {};

	using CmdDerive::CmdDerive;
	std::string_view nameDesc() const override { return "DrawMeshTasksIndirect"; };
	void record(const Device&, VkCommandBuffer, u32) const override;
	bool isIndexed() const override { return false; }
};

struct DrawMeshTasksIndirectCountCmd final : CmdDerive<DrawCmdBase, CommandType::drawMeshTasksIndirectCount> {
	Buffer* buffer {};
	VkDeviceSize offset {};
	Buffer* countBuffer {};
	VkDeviceSize countOffset {};
	u32 maxDrawCount {};
	u32 stride {};

	using CmdDerive::CmdDerive;
	std::string_view nameDesc() const override { return "DrawMeshTasksIndirectCount"; };
	void record(const Device&, VkCommandBuffer, u32) const override;
	bool isIndexed() const override { return false; }
};

struct BindVertexBuffersCmd final : CmdDerive<Command, CommandType::bindVertexBuffers> {
	u32 firstBinding;
	span<BoundVertexBuffer> buffers;
	bool v2 {}; // whether CmdBindVertexBuffers2 was used.
	bool strides {}; // whether strides were passed
	bool sizes {}; // whether sizes were passed

	std::string toString() const override;
	void displayInspector(Gui&) const override;

	std::string_view nameDesc() const override { return v2 ? "BindVertexBuffers2" : "BindVertexBuffers"; }
	Category category() const override { return Category::bind; }
	void record(const Device&, VkCommandBuffer, u32) const override;
};

struct BindIndexBufferCmd final : CmdDerive<Command, CommandType::bindIndexBuffer> {
	Buffer* buffer {};
	VkDeviceSize offset {};
	VkIndexType indexType {};
	std::optional<VkDeviceSize> size {}; // for vkCmdBindIndexBuffer2KHR

	std::string_view nameDesc() const override {
		return size ? "BindIndexBuffer2" : "BindIndexBuffer";
	}
	Category category() const override { return Category::bind; }
	void record(const Device&, VkCommandBuffer, u32) const override;
};

struct BindDescriptorSetCmd final : CmdDerive<Command, CommandType::bindDescriptorSet> {
	u32 firstSet;
	PipelineLayout* pipeLayout; // kept alive via shared_ptr in CommandBuffer
	span<DescriptorSet*> sets; // NOTE: handles may be invalid
	span<u32> dynamicOffsets;

	VkPipelineBindPoint pipeBindPoint {}; // only relevant for BindDescriptorSets
	VkShaderStageFlags stageFlags {}; // only relevant for BindDescriptorSets2

	std::string toString() const override;
	std::string_view nameDesc() const override {
		return stageFlags ? "BindDescriptorSets2" : "BindDescriptorSets";
	}
	Category category() const override { return Category::bind; }
	void record(const Device&, VkCommandBuffer, u32) const override;
	void displayInspector(Gui& gui) const override;
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
	Pipeline* boundPipe() const override { return state->pipe; }
	const PushConstantData& boundPushConstants() const override { return pushConstants; }
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
};

struct DispatchIndirectCmd final : CmdDerive<DispatchCmdBase, CommandType::dispatchIndirect> {
	Buffer* buffer {};
	VkDeviceSize offset {};

	using CmdDerive::CmdDerive;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "DispatchIndirect"; }
	void record(const Device&, VkCommandBuffer, u32) const override;
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
};

struct ClearAttachmentCmd final : CmdDerive<Command, CommandType::clearAttachment> {
	span<VkClearAttachment> attachments;
	span<VkClearRect> rects;

	std::string_view nameDesc() const override { return "ClearAttachment"; }
	void displayInspector(Gui& gui) const override;
	// NOTE: strictly speaking this isn't a transfer, we don't care.
	Category category() const override { return Category::transfer; }
	void record(const Device&, VkCommandBuffer, u32) const override;
};

struct SetEventCmd final : CmdDerive<Command, CommandType::setEvent> {
	Event* event {};
	VkPipelineStageFlags stageMask {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "SetEvent"; }
	Category category() const override { return Category::sync; }
	void record(const Device&, VkCommandBuffer, u32) const override;
};

struct SetEvent2Cmd final : CmdDerive<Barrier2CmdBase, CommandType::setEvent2> {
	Event* event {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string_view nameDesc() const override { return "SetEvent"; }
	Category category() const override { return Category::sync; }
	void record(const Device&, VkCommandBuffer, u32) const override;
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
};

struct EndDebugUtilsLabelCmd final : CmdDerive<Command, CommandType::endDebugUtilsLabel> {
	std::string_view nameDesc() const override { return "EndLabel"; }
	Category category() const override { return Category::end; }
	void record(const Device&, VkCommandBuffer, u32) const override;
};

struct InsertDebugUtilsLabelCmd final : CmdDerive<Command, CommandType::insertDebugUtilsLabel> {
	const char* name {};
	std::array<float, 4> color; // NOTE: could use this in UI

	std::string_view nameDesc() const override { return name; }
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
};

struct PushConstantsCmd final : CmdDerive<Command, CommandType::pushConstants> {
	PipelineLayout* pipeLayout; // kept alive via shared_ptr in CommandBuffer
	VkShaderStageFlags stages {};
	u32 offset {};
	span<std::byte> values;

	std::string_view nameDesc() const override { return "PushConstants"; }
	Category category() const override { return Category::bind; }
	void record(const Device&, VkCommandBuffer, u32) const override;
};

// dynamic state
struct SetViewportCmd final : CmdDerive<Command, CommandType::setViewport> {
	u32 first {};
	span<VkViewport> viewports;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetViewport"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void displayInspector(Gui& gui) const override;
};

struct SetScissorCmd final : CmdDerive<Command, CommandType::setScissor> {
	u32 first {};
	span<VkRect2D> scissors;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetScissor"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void displayInspector(Gui& gui) const override;
};

struct SetLineWidthCmd final : CmdDerive<Command, CommandType::setLineWidth> {
	float width {};

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetLineWidth"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void displayInspector(Gui& gui) const override;
};

struct SetDepthBiasCmd final : CmdDerive<Command, CommandType::setDepthBias> {
	DynamicStateDepthBias state {};

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetDepthBias"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct SetDepthBoundsCmd final : CmdDerive<Command, CommandType::setDepthBounds> {
	float min {};
	float max {};

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetDepthBounds"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct SetBlendConstantsCmd final : CmdDerive<Command, CommandType::setBlendConstants> {
	std::array<float, 4> values {};

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetBlendConstants"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct SetStencilCompareMaskCmd final : CmdDerive<Command, CommandType::setStencilCompareMask> {
	VkStencilFaceFlags faceMask {};
	u32 value {};

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetStencilCompareMask"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct SetStencilWriteMaskCmd final : CmdDerive<Command, CommandType::setStencilWriteMask> {
	VkStencilFaceFlags faceMask {};
	u32 value {};

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetStencilWriteMask"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct SetStencilReferenceCmd final : CmdDerive<Command, CommandType::setStencilReference> {
	VkStencilFaceFlags faceMask {};
	u32 value {};

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetStencilReference"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

// query pool
struct BeginQueryCmd final : CmdDerive<Command, CommandType::beginQuery> {
	QueryPool* pool {};
	u32 query {};
	VkQueryControlFlags flags {};

	Category category() const override { return Category::query; }
	std::string_view nameDesc() const override { return "BeginQuery"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct EndQueryCmd final : CmdDerive<Command, CommandType::endQuery> {
	QueryPool* pool {};
	u32 query {};

	Category category() const override { return Category::query; }
	std::string_view nameDesc() const override { return "EndQuery"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct ResetQueryPoolCmd final : CmdDerive<Command, CommandType::resetQueryPool> {
	QueryPool* pool {};
	u32 first {};
	u32 count {};

	Category category() const override { return Category::query; }
	std::string_view nameDesc() const override { return "ResetQueryPool"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
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
};

// Push descriptor commands
struct PushDescriptorSetCmd final : CmdDerive<Command, CommandType::pushDescriptorSet> {
	PipelineLayout* pipeLayout {};
	u32 set {};

	VkPipelineBindPoint bindPoint {}; // only relevant for PushDescriptorSet
	VkShaderStageFlags stages {}; // only relevant for PushDescriptorSet2

	// The individual pImageInfo, pBufferInfo, pTexelBufferView arrays
	// are allocated in the CommandRecord-owned memory as well.
	span<VkWriteDescriptorSet> descriptorWrites;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override {
		return stages ? "PushDescriptorSet2" : "PushDescriptorSet";
	}
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	void displayInspector(Gui& gui) const override;
};

struct PushDescriptorSetWithTemplateCmd final : CmdDerive<Command, CommandType::pushDescriptorSetWithTemplate> {
	DescriptorUpdateTemplate* updateTemplate {};
	PipelineLayout* pipeLayout {};
	u32 set {};
	span<const std::byte> data;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "PushDescriptorSetWithTemplate"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

// VK_KHR_fragment_shading_rate
struct SetFragmentShadingRateCmd final : CmdDerive<Command, CommandType::setFragmentShadingRate> {
	VkExtent2D fragmentSize;
	std::array<VkFragmentShadingRateCombinerOpKHR, 2> combinerOps;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetFragmentShadingRateCmd"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

// VK_EXT_conditional_rendering
struct BeginConditionalRenderingCmd final : CmdDerive<SectionCommand, CommandType::beginConditionalRendering> {
	Buffer* buffer;
	VkDeviceSize offset;
	VkConditionalRenderingFlagsEXT flags;

	Category category() const override { return Category::other; }
	std::string_view nameDesc() const override { return "BeginConditionalRendering"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct EndConditionalRenderingCmd final : CmdDerive<Command, CommandType::endConditionalRendering> {
	Category category() const override { return Category::end; }
	std::string_view nameDesc() const override { return "EndConditionalRendering"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

// VK_EXT_line_rasterization
struct SetLineStippleCmd final : CmdDerive<Command, CommandType::setLineStipple> {
	u32 stippleFactor;
	u16 stipplePattern;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetLineStipple"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

// VK_EXT_extended_dynamic_state
struct SetCullModeCmd final : CmdDerive<Command, CommandType::setCullMode> {
	VkCullModeFlags cullMode;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetCullMode"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct SetFrontFaceCmd final : CmdDerive<Command, CommandType::setFrontFace> {
	VkFrontFace frontFace;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetFrontFace"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct SetPrimitiveTopologyCmd final : CmdDerive<Command, CommandType::setPrimitiveTopology> {
	VkPrimitiveTopology topology;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetPrimitiveTopology"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct SetViewportWithCountCmd final : CmdDerive<Command, CommandType::setViewportWithCount> {
	span<VkViewport> viewports;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetViewportWithCount"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct SetScissorWithCountCmd final : CmdDerive<Command, CommandType::setScissorWithCount> {
	span<VkRect2D> scissors;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetScissorWithCount"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

// BindVertexBuffers2Cmd is mapped via BindVertexBuffers

struct SetDepthTestEnableCmd final : CmdDerive<Command, CommandType::setDepthTestEnable> {
	bool enable;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetDepthTestEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct SetDepthWriteEnableCmd final : CmdDerive<Command, CommandType::setDepthWriteEnable> {
	bool enable;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetDepthWriteEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct SetDepthCompareOpCmd final : CmdDerive<Command, CommandType::setDepthCompareOp> {
	VkCompareOp op;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetDepthCompareOp"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct SetDepthBoundsTestEnableCmd final : CmdDerive<Command, CommandType::setDepthBoundsTestEnable> {
	bool enable;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetDepthBoundsTestEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct SetStencilTestEnableCmd final : CmdDerive<Command, CommandType::setStencilTestEnable> {
	bool enable;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetStencilTestEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
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
};

// VK_EXT_extended_dynamic_state2
struct SetPatchControlPointsCmd final : CmdDerive<Command, CommandType::setPatchControlPoints> {
	u32 patchControlPoints;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetPatchControlPoints"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct SetRasterizerDiscardEnableCmd final : CmdDerive<Command, CommandType::setRasterizerDiscardEnable> {
	bool enable;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetRasterizerDiscardEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct SetDepthBiasEnableCmd final : CmdDerive<Command, CommandType::setDepthBias> {
	bool enable;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetDepthBiasEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct SetLogicOpCmd final : CmdDerive<Command, CommandType::setLogicOp> {
	VkLogicOp logicOp;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetLogicOp"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct SetPrimitiveRestartEnableCmd final : CmdDerive<Command, CommandType::setPrimitiveRestartEnable> {
	bool enable;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetPrimitiveRestartEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

// VK_EXT_sample_locations
struct SetSampleLocationsCmd final : CmdDerive<Command, CommandType::setSampleLocations> {
	VkSampleLocationsInfoEXT info;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetSampleLocations"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

// VK_EXT_discard_rectangles
struct SetDiscardRectangleCmd final : CmdDerive<Command, CommandType::setDisacrdRectangle> {
	u32 first;
	span<VkRect2D> rects;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetDiscardRectangle"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
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
};

struct CopyAccelStructToMemoryCmd final : CmdDerive<Command, CommandType::copyAccelStructToMemory> {
	const void* pNext {};
	AccelStruct* src {};
    VkDeviceOrHostAddressKHR dst {};
    VkCopyAccelerationStructureModeKHR mode;

	Category category() const override { return Category::transfer; }
	std::string_view nameDesc() const override { return "CopyAccelerationStructureToMemory"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct CopyMemoryToAccelStructCmd final : CmdDerive<Command, CommandType::copyMemoryToAccelStruct> {
	const void* pNext {};
    VkDeviceOrHostAddressConstKHR src {};
	AccelStruct* dst {};
    VkCopyAccelerationStructureModeKHR mode;

	Category category() const override { return Category::transfer; }
	std::string_view nameDesc() const override { return "CopyMemoryToAccelerationStructure"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct WriteAccelStructsPropertiesCmd final : CmdDerive<Command, CommandType::writeAccelStructsProperties> {
	span<AccelStruct*> accelStructs;
	VkQueryType queryType {};
	QueryPool* queryPool {};
	u32 firstQuery {};

	Category category() const override { return Category::query; }
	std::string_view nameDesc() const override { return "WriteAccelerationStructuresProperties"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
};

struct BuildAccelStructsCmd final : CmdDerive<Command, CommandType::buildAccelStructs> {
	span<VkAccelerationStructureBuildGeometryInfoKHR> buildInfos; // already handle-fwd-patched
	span<span<VkAccelerationStructureBuildRangeInfoKHR>> buildRangeInfos;
	span<AccelStruct*> srcs;
	span<AccelStruct*> dsts;

	Category category() const override { return Category::buildAccelStruct; }
	std::string_view nameDesc() const override { return "BuildAccelerationStructures"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
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
	Pipeline* boundPipe() const override { return state->pipe; }
	const PushConstantData& boundPushConstants() const override { return pushConstants; }
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
	void displayInspector(Gui& gui) const override;
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
};

struct SetRayTracingPipelineStackSizeCmd final : CmdDerive<Command, CommandType::setRayTracingPipelineStackSize> {
	u32 stackSize;

	Category category() const override { return Category::bind; }
	std::string_view nameDesc() const override { return "SetRayTracingPipelineStackSize"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
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

	// TODO: inspector
	// TODO: toString?
};

struct EndRenderingCmd final : CmdDerive<Command, CommandType::endRendering> {
	std::string_view nameDesc() const override { return "EndRendering"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::end; }
};

// VK_EXT_vertex_input_dynamic_state
struct SetVertexInputCmd final : CmdDerive<Command, CommandType::setVertexInput> {
	span<VkVertexInputBindingDescription2EXT> bindings;
	span<VkVertexInputAttributeDescription2EXT> attribs;

	std::string_view nameDesc() const override { return "SetVertexInput"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

// VK_EXT_color_write_enable
struct SetColorWriteEnableCmd final : CmdDerive<Command, CommandType::setColorWriteEnable> {
	span<VkBool32> writeEnables;

	std::string_view nameDesc() const override { return "SetColorWriteEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

// VK_EXT_shader_object
struct BindShadersCmd final : CmdDerive<Command, CommandType::bindShaders> {
	span<VkShaderStageFlagBits> stages;
	span<ShaderObject*> shaders;

	std::string_view nameDesc() const override { return "BindShadersCmd"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetDepthClampRangeCmd final : CmdDerive<Command, CommandType::setDepthClampRange> {
	VkDepthClampModeEXT mode;
	VkDepthClampRangeEXT range {};

	std::string_view nameDesc() const override { return "SetDepthClampRange"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

// VK_EXT_device_generated_commands
// VkGeneratedCommandsInfoEXT, unwrapped
struct GeneratedCommandsInfo {
	VkShaderStageFlags stages {};
	IndirectExecutionSet* execSet {};
	IndirectCommandsLayout* layout {};
	VkDeviceAddress indirectAddress {};
	VkDeviceSize indirectSize {};
	VkDeviceAddress preprocessAddress {};
	VkDeviceSize preprocessSize {};
	u32 maxSequenceCount {};
	VkDeviceAddress sequenceCountAddress {};
	u32 maxDrawCount {};
};

// TODO: make this parent command?
struct ExecuteGeneratedCommandsCmd final : CmdDerive<Command, CommandType::executeGeneratedCommands> {
	bool isPreprocessed {};
	GeneratedCommandsInfo info {};

	std::string_view nameDesc() const override { return "ExecuteGeneratedCommands"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::other; }
};

struct PreprocessGeneratedCommandsCmd final : CmdDerive<Command, CommandType::preprocessGeneratedCommands> {
	GeneratedCommandsInfo info {};
	CommandBuffer* state {};

	std::string_view nameDesc() const override { return "PreprocessGeneratedCommands"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::other; }
};

// VK_EXT_extended_dynamic_state3
struct SetDepthClampEnableCmd final : CmdDerive<Command, CommandType::setDepthClampEnable> {
	bool enable {};

	std::string_view nameDesc() const override { return "SetDepthClampEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetPolygonModeCmd final : CmdDerive<Command, CommandType::setPolygonMode> {
	VkPolygonMode mode {};

	std::string_view nameDesc() const override { return "SetPolygonMode"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetRasterizationSamplesCmd final : CmdDerive<Command, CommandType::setRasterizationSamples> {
	VkSampleCountFlagBits samples {};

	std::string_view nameDesc() const override { return "SetRasterizationSamples"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetSampleMaskCmd final : CmdDerive<Command, CommandType::setSampleMask> {
	VkSampleCountFlagBits samples {};
	span<VkSampleMask> sampleMask;

	std::string_view nameDesc() const override { return "SetSampleMask"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetAlphaToCoverageEnableCmd final : CmdDerive<Command, CommandType::setAlphaToCoverageEnable> {
	bool enable {};

	std::string_view nameDesc() const override { return "SetAlphaToCoverageEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetAlphaToOneEnableCmd final : CmdDerive<Command, CommandType::setAlphaToOneEnable> {
	bool enable {};

	std::string_view nameDesc() const override { return "SetAlphaToOneEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetLogicOpEnableCmd final : CmdDerive<Command, CommandType::setLogicOpEnable> {
	bool enable {};

	std::string_view nameDesc() const override { return "SetLogicOpEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetColorBlendEnableCmd final : CmdDerive<Command, CommandType::setColorBlendEnable> {
	u32 firstAttachment {};
	span<VkBool32> enable;

	std::string_view nameDesc() const override { return "SetColorBlendEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetColorBlendEquationCmd final : CmdDerive<Command, CommandType::setColorBlendEquation> {
	u32 firstAttachment {};
	span<VkColorBlendEquationEXT> equations;

	std::string_view nameDesc() const override { return "SetColorBlendEquation"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetColorWriteMaskCmd final : CmdDerive<Command, CommandType::setColorWriteMask> {
	u32 firstAttachment {};
	span<VkColorComponentFlags> colorWriteMasks;

	std::string_view nameDesc() const override { return "SetColorWriteMask"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetTessellationDomainOriginCmd final : CmdDerive<Command, CommandType::setTessellationDomainOrigin> {
	VkTessellationDomainOrigin domainOrigin;

	std::string_view nameDesc() const override { return "SetTessellationDomainOrigin"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetRasterizationStreamCmd final : CmdDerive<Command, CommandType::setRasterizationStream> {
	u32 rasterizationStream {};

	std::string_view nameDesc() const override { return "SetRasterizationStream"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetConservativeRasterizationModeCmd final : CmdDerive<Command, CommandType::setConservativeRasterizationMode> {
	VkConservativeRasterizationModeEXT mode {};

	std::string_view nameDesc() const override { return "SetConservativeRasterizationMode"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetExtraPrimitiveOverestimationSizeCmd final : CmdDerive<Command, CommandType::setExtraPrimitiveOverestimationSize> {
	float extraPrimitiveOverestimationSize {};

	std::string_view nameDesc() const override { return "SetExtraPrimitiveOverestimationSize"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetDepthClipEnableCmd final : CmdDerive<Command, CommandType::setDepthClipEnable> {
	bool enable {};

	std::string_view nameDesc() const override { return "SetDepthClipEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetSampleLocationsEnableCmd final : CmdDerive<Command, CommandType::setSampleLocationsEnable> {
	bool enable {};

	std::string_view nameDesc() const override { return "SetSampleLocationsEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetColorBlendAdvancedCmd final : CmdDerive<Command, CommandType::setColorBlendAdvanced> {
	u32 firstAttachment {};
	span<VkColorBlendAdvancedEXT> blend;

	std::string_view nameDesc() const override { return "SetColorBlendAdvanced"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetProvokingVertexModeCmd final : CmdDerive<Command, CommandType::setProvokingVertexMode> {
	VkProvokingVertexModeEXT mode {};

	std::string_view nameDesc() const override { return "SetProvokingVertexMode"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetLineRasterizationModeCmd final : CmdDerive<Command, CommandType::setLineRasterizationMode> {
	VkLineRasterizationModeEXT mode {};

	std::string_view nameDesc() const override { return "SetLineRasterizationMode"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetLineStippleEnableCmd final : CmdDerive<Command, CommandType::setLineStippleEnable> {
	bool enable {};

	std::string_view nameDesc() const override { return "SetLineStippleEnable"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

struct SetDepthClipNegativeOneToOneEXTCmd final : CmdDerive<Command, CommandType::setDepthClipNegativeOneToOneEXT> {
	bool enable {};

	std::string_view nameDesc() const override { return "SetDepthClipNegativeOneToOneEXT"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};

// VK_EXT_depth_bias_control
struct SetDepthBias2Cmd final : CmdDerive<Command, CommandType::setDepthBias2> {
    float depthBiasConstantFactor {};
    float depthBiasClamp {};
    float depthBiasSlopeFactor {};
	const void* pNext {};

	std::string_view nameDesc() const override { return "SetDepthBias2EXT"; }
	void record(const Device&, VkCommandBuffer cb, u32) const override;
	Category category() const override { return Category::bind; }
};


// F: overloaded function type of signature void(<Command Types>*)
// Will call f with cmd casted to the type indicated by cmdType.
// Can be used to implement the visitor pattern, as the overloaded function
// could also just accept base classes.
// Can be called with cmd = nullptr, f will just get a nullptr of the type
// representing 'cmdType' then. Otherwise cmdType must match cmd->type() to
// not cast incorrectly.
template<typename F>
auto castCommandType(CommandType cmdType, Command* cmd, F&& f) {
	using CT = CommandType;

	switch(cmdType) {
	case CT::root: return f(static_cast<RootCommand*>(cmd));
	case CT::waitEvents: return f(static_cast<WaitEventsCmd*>(cmd));
	case CT::waitEvents2: return f(static_cast<WaitEvents2Cmd*>(cmd));
	case CT::barrier: return f(static_cast<BarrierCmd*>(cmd));
	case CT::barrier2: return f(static_cast<Barrier2Cmd*>(cmd));
	case CT::beginRenderPass: return f(static_cast<BeginRenderPassCmd*>(cmd));
	case CT::nextSubpass: return f(static_cast<NextSubpassCmd*>(cmd));
	case CT::firstSubpass: return f(static_cast<FirstSubpassCmd*>(cmd));
	case CT::endRenderPass: return f(static_cast<EndRenderPassCmd*>(cmd));
	case CT::draw: return f(static_cast<DrawCmd*>(cmd));
	case CT::drawIndirect: return f(static_cast<DrawIndirectCmd*>(cmd));
	case CT::drawIndexed: return f(static_cast<DrawIndexedCmd*>(cmd));
	case CT::drawIndirectCount: return f(static_cast<DrawIndirectCountCmd*>(cmd));
	case CT::drawMulti: return f(static_cast<DrawMultiCmd*>(cmd));
	case CT::drawMultiIndexed: return f(static_cast<DrawMultiIndexedCmd*>(cmd));
	case CT::bindVertexBuffers: return f(static_cast<BindVertexBuffersCmd*>(cmd));
	case CT::bindIndexBuffer: return f(static_cast<BindIndexBufferCmd*>(cmd));
	case CT::bindDescriptorSet: return f(static_cast<BindDescriptorSetCmd*>(cmd));
	case CT::dispatch: return f(static_cast<DispatchCmd*>(cmd));
	case CT::dispatchIndirect: return f(static_cast<DispatchIndirectCmd*>(cmd));
	case CT::dispatchBase: return f(static_cast<DispatchBaseCmd*>(cmd));
	case CT::copyImage: return f(static_cast<CopyImageCmd*>(cmd));
	case CT::copyBufferToImage: return f(static_cast<CopyBufferToImageCmd*>(cmd));
	case CT::copyImageToBuffer: return f(static_cast<CopyImageToBufferCmd*>(cmd));
	case CT::blitImage: return f(static_cast<BlitImageCmd*>(cmd));
	case CT::resolveImage: return f(static_cast<ResolveImageCmd*>(cmd));
	case CT::copyBuffer: return f(static_cast<CopyBufferCmd*>(cmd));
	case CT::updateBuffer: return f(static_cast<UpdateBufferCmd*>(cmd));
	case CT::fillBuffer: return f(static_cast<FillBufferCmd*>(cmd));
	case CT::clearColorImage: return f(static_cast<ClearColorImageCmd*>(cmd));
	case CT::clearDepthStencilImage: return f(static_cast<ClearDepthStencilImageCmd*>(cmd));
	case CT::clearAttachment: return f(static_cast<ClearAttachmentCmd*>(cmd));
	case CT::setEvent: return f(static_cast<SetEventCmd*>(cmd));
	case CT::setEvent2: return f(static_cast<SetEvent2Cmd*>(cmd));
	case CT::resetEvent: return f(static_cast<ResetEventCmd*>(cmd));
	case CT::executeCommandsChild: return f(static_cast<ExecuteCommandsChildCmd*>(cmd));
	case CT::executeCommands: return f(static_cast<ExecuteCommandsCmd*>(cmd));
	case CT::beginDebugUtilsLabel: return f(static_cast<BeginDebugUtilsLabelCmd*>(cmd));
	case CT::endDebugUtilsLabel: return f(static_cast<EndDebugUtilsLabelCmd*>(cmd));
	case CT::insertDebugUtilsLabel: return f(static_cast<InsertDebugUtilsLabelCmd*>(cmd));
	case CT::bindPipeline: return f(static_cast<BindPipelineCmd*>(cmd));
	case CT::pushConstants: return f(static_cast<PushConstantsCmd*>(cmd));
	case CT::setViewport: return f(static_cast<SetViewportCmd*>(cmd));
	case CT::setScissor: return f(static_cast<SetScissorCmd*>(cmd));
	case CT::setLineWidth: return f(static_cast<SetLineWidthCmd*>(cmd));
	case CT::setDepthBias: return f(static_cast<SetDepthBiasCmd*>(cmd));
	case CT::setDepthBounds: return f(static_cast<SetDepthBoundsCmd*>(cmd));
	case CT::setBlendConstants: return f(static_cast<SetBlendConstantsCmd*>(cmd));
	case CT::setStencilCompareMask: return f(static_cast<SetStencilCompareMaskCmd*>(cmd));
	case CT::setStencilWriteMask: return f(static_cast<SetStencilWriteMaskCmd*>(cmd));
	case CT::setStencilReference: return f(static_cast<SetStencilReferenceCmd*>(cmd));
	case CT::beginQuery: return f(static_cast<BeginQueryCmd*>(cmd));
	case CT::endQuery: return f(static_cast<EndQueryCmd*>(cmd));
	case CT::resetQueryPool: return f(static_cast<ResetQueryPoolCmd*>(cmd));
	case CT::writeTimestamp: return f(static_cast<WriteTimestampCmd*>(cmd));
	case CT::copyQueryPoolResults: return f(static_cast<CopyQueryPoolResultsCmd*>(cmd));
	case CT::pushDescriptorSet: return f(static_cast<PushDescriptorSetCmd*>(cmd));
	case CT::pushDescriptorSetWithTemplate: return f(static_cast<PushDescriptorSetWithTemplateCmd*>(cmd));
	case CT::setFragmentShadingRate: return f(static_cast<SetFragmentShadingRateCmd*>(cmd));
	case CT::beginConditionalRendering: return f(static_cast<BeginConditionalRenderingCmd*>(cmd));
	case CT::endConditionalRendering: return f(static_cast<EndConditionalRenderingCmd*>(cmd));
	case CT::setLineStipple: return f(static_cast<SetLineStippleCmd*>(cmd));
	case CT::setCullMode: return f(static_cast<SetCullModeCmd*>(cmd));
	case CT::setFrontFace: return f(static_cast<SetFrontFaceCmd*>(cmd));
	case CT::setPrimitiveTopology: return f(static_cast<SetPrimitiveTopologyCmd*>(cmd));
	case CT::setViewportWithCount: return f(static_cast<SetViewportWithCountCmd*>(cmd));
	case CT::setScissorWithCount: return f(static_cast<SetScissorWithCountCmd*>(cmd));
	case CT::setDepthTestEnable: return f(static_cast<SetDepthTestEnableCmd*>(cmd));
	case CT::setDepthWriteEnable: return f(static_cast<SetDepthWriteEnableCmd*>(cmd));
	case CT::setDepthCompareOp: return f(static_cast<SetDepthCompareOpCmd*>(cmd));
	case CT::setDepthBoundsTestEnable: return f(static_cast<SetDepthBoundsTestEnableCmd*>(cmd));
	case CT::setStencilTestEnable: return f(static_cast<SetStencilTestEnableCmd*>(cmd));
	case CT::setStencilOp: return f(static_cast<SetStencilOpCmd*>(cmd));
	case CT::setPatchControlPoints: return f(static_cast<SetPatchControlPointsCmd*>(cmd));
	case CT::setRasterizerDiscardEnable: return f(static_cast<SetRasterizerDiscardEnableCmd*>(cmd));
	case CT::setDepthBiasEnable: return f(static_cast<SetDepthBiasEnableCmd*>(cmd));
	case CT::setLogicOp: return f(static_cast<SetLogicOpCmd*>(cmd));
	case CT::setPrimitiveRestartEnable: return f(static_cast<SetPrimitiveRestartEnableCmd*>(cmd));
	case CT::setSampleLocations: return f(static_cast<SetSampleLocationsCmd*>(cmd));
	case CT::setDisacrdRectangle: return f(static_cast<SetDiscardRectangleCmd*>(cmd));
	case CT::copyAccelStruct: return f(static_cast<CopyAccelStructCmd*>(cmd));
	case CT::copyAccelStructToMemory: return f(static_cast<CopyAccelStructToMemoryCmd*>(cmd));
	case CT::copyMemoryToAccelStruct: return f(static_cast<CopyMemoryToAccelStructCmd*>(cmd));
	case CT::writeAccelStructsProperties: return f(static_cast<WriteAccelStructsPropertiesCmd*>(cmd));
	case CT::buildAccelStructs: return f(static_cast<BuildAccelStructsCmd*>(cmd));
	case CT::buildAccelStructsIndirect: return f(static_cast<BuildAccelStructsIndirectCmd*>(cmd));
	case CT::traceRays: return f(static_cast<TraceRaysCmd*>(cmd));
	case CT::traceRaysIndirect: return f(static_cast<TraceRaysIndirectCmd*>(cmd));
	case CT::setRayTracingPipelineStackSize: return f(static_cast<SetRayTracingPipelineStackSizeCmd*>(cmd));
	case CT::beginRendering: return f(static_cast<BeginRenderingCmd*>(cmd));
	case CT::endRendering: return f(static_cast<EndRenderingCmd*>(cmd));
	case CT::setVertexInput: return f(static_cast<SetVertexInputCmd*>(cmd));
	case CT::setColorWriteEnable: return f(static_cast<SetColorWriteEnableCmd*>(cmd));
	case CT::bindShaders: return f(static_cast<BindShadersCmd*>(cmd));
	case CT::setDepthClampRange: return f(static_cast<SetDepthClampRangeCmd*>(cmd));
	case CT::drawMeshTasks: return f(static_cast<DrawMeshTasksCmd*>(cmd));
	case CT::drawMeshTasksIndirect: return f(static_cast<DrawMeshTasksIndirectCmd*>(cmd));
	case CT::drawMeshTasksIndirectCount: return f(static_cast<DrawMeshTasksIndirectCountCmd*>(cmd));
	case CT::executeGeneratedCommands: return f(static_cast<ExecuteGeneratedCommandsCmd*>(cmd));
	case CT::preprocessGeneratedCommands: return f(static_cast<PreprocessGeneratedCommandsCmd*>(cmd));
	case CT::setDepthClampEnable: return f(static_cast<SetDepthClampEnableCmd*>(cmd));
	case CT::setPolygonMode: return f(static_cast<SetPolygonModeCmd*>(cmd));
	case CT::setRasterizationSamples: return f(static_cast<SetRasterizationSamplesCmd*>(cmd));
	case CT::setSampleMask: return f(static_cast<SetSampleMaskCmd*>(cmd));
	case CT::setAlphaToCoverageEnable: return f(static_cast<SetAlphaToCoverageEnableCmd*>(cmd));
	case CT::setAlphaToOneEnable: return f(static_cast<SetAlphaToOneEnableCmd*>(cmd));
	case CT::setLogicOpEnable: return f(static_cast<SetLogicOpEnableCmd*>(cmd));
	case CT::setColorBlendEnable: return f(static_cast<SetColorBlendEnableCmd*>(cmd));
	case CT::setColorBlendEquation: return f(static_cast<SetColorBlendEquationCmd*>(cmd));
	case CT::setColorWriteMask: return f(static_cast<SetColorWriteMaskCmd*>(cmd));
	case CT::setTessellationDomainOrigin: return f(static_cast<SetTessellationDomainOriginCmd*>(cmd));
	case CT::setRasterizationStream: return f(static_cast<SetRasterizationStreamCmd*>(cmd));
	case CT::setConservativeRasterizationMode: return f(static_cast<SetConservativeRasterizationModeCmd*>(cmd));
	case CT::setExtraPrimitiveOverestimationSize: return f(static_cast<SetExtraPrimitiveOverestimationSizeCmd*>(cmd));
	case CT::setDepthClipEnable: return f(static_cast<SetDepthClipEnableCmd*>(cmd));
	case CT::setSampleLocationsEnable: return f(static_cast<SetSampleLocationsEnableCmd*>(cmd));
	case CT::setColorBlendAdvanced: return f(static_cast<SetColorBlendAdvancedCmd*>(cmd));
	case CT::setProvokingVertexMode: return f(static_cast<SetProvokingVertexModeCmd*>(cmd));
	case CT::setLineRasterizationMode: return f(static_cast<SetLineRasterizationModeCmd*>(cmd));
	case CT::setLineStippleEnable: return f(static_cast<SetLineStippleEnableCmd*>(cmd));
	case CT::setDepthClipNegativeOneToOneEXT: return f(static_cast<SetDepthClipNegativeOneToOneEXTCmd*>(cmd));
	case CT::setDepthBias2: return f(static_cast<SetDepthBias2Cmd*>(cmd));
	case CT::count:
		dlg_error("Invalid command type");
	// NOTE: no default case by design so that we get compiler warnings about
	// missing cases here.
	}
}

template<typename F>
void castCommandType(Command& cmd, F&& f) {
	return castCommandType(cmd.type(), &cmd, f);
}

template<typename F>
void castCommandType(const Command& cmd, F&& f) {
	return castCommandType(cmd.type(), const_cast<Command*>(&cmd), f);
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
