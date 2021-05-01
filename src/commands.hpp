#pragma once

#include <record.hpp>
#include <util/flags.hpp>
#include <util/span.hpp>
#include <dlg/dlg.hpp>

// See ~Command
#ifdef __GNUC__
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif // __GNUC__

namespace vil {

// The type of a command is used e.g. to hide them in the UI.
enum class CommandType : u32 {
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
};

using CommandTypeFlags = nytl::Flags<CommandType>;

// The list of commands in a CommandBuffer is organized as a tree.
// Section-like commands (e.g. cmdBeginRenderPass) have all their commands
// stored as children
struct Command {
	using Type = CommandType;
	using TypeFlags = CommandTypeFlags;

	// NOTE: Commands should never have a non-trivial destructor (that is
	// static_assert'd in addCmd) since it won't be called. We do this
	// so that resetting command buffers does not add significant overhead
	// as we can have *a lot* of commands in a command buffer.
	// When special commands need resources with destructor, those should
	// be separately stored in the command buffer.
	~Command() = default;

	// Display a one-line overview of the command via ImGui.
	// Commands with children should display themselves as tree nodes.
	// Gets the command that is currently selected and should return
	// the hierachy of children selected in this frame (or an empty vector
	// if none is).
	virtual std::vector<const Command*> display(const Command* sel, TypeFlags typeFlags) const;

	// The name of the command as string (might include parameter information).
	// Used by the default 'display' implementation.
	virtual std::string toString() const { return nameDesc(); }

	// Should return the most important arguments as strings.
	// Used to build a CommandDescription.
	virtual std::vector<std::string> argumentsDesc() const { return {}; }
	virtual std::string nameDesc() const { return "<unknown>"; }

	// Whether this command is empty. Empty commands are usually hidden.
	// Used for commands like CmdEndRenderPass.
	virtual Type type() const { return Type::other; }

	// Records itself into the given command buffer.
	// Does not record its child commands (if it has any).
	virtual void record(const Device& dev, VkCommandBuffer) const = 0;

	// Draws the command inspector UI.
	// Gets a list of destroyed handled, might include referenced handles
	// by this command. Must not dereference them.
	virtual void displayInspector(Gui&) const {}

	// Informs this command that the given handles were destroyed.
	// Should unset all internal handles that are among them.
	// Should forward the call to all potential children.
	virtual void unset(const std::unordered_set<DeviceHandle*>&) {}

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

	// Returns how much this commands matches with the given one.
	// Should always return 0.f for two commands that don't have the
	// same type. Should not consider child commands, just itself.
	virtual float match(const Command& cmd) const;

	// Forms a forward linked list with siblings
	Command* next {};

	// How many sibilings with same nameDesc() came before this in parent
	// NOTE: i don't like this here. Not sure how to properly solve this.
	// It's needed by displayCommands to generate proper imgui ids
	unsigned relID {};
};

NYTL_FLAG_OPS(Command::Type)

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

NameResult name(DeviceHandle*, NullName = NullName::null);
std::vector<const Command*> displayCommands(const Command* cmd,
		const Command* selected, Command::TypeFlags typeFlags);

struct Matcher;

struct ParentCommand : Command {
	std::vector<const Command*> display(const Command* selected,
		TypeFlags typeFlags, const Command* cmd) const;
	std::vector<const Command*> display(const Command* selected,
		TypeFlags typeFlags) const override;

	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override {
		auto* cmd = children();
		while(cmd) {
			cmd->unset(destroyed);
			cmd = cmd->next;
		}
	}
};

struct SectionCommand : ParentCommand {
	Command* children_ {};
	Command* children() const override { return children_; }
};

struct BarrierCmdBase : Command {
    VkPipelineStageFlags srcStageMask {};
    VkPipelineStageFlags dstStageMask {};

	span<VkMemoryBarrier> memBarriers;
	span<VkBufferMemoryBarrier> bufBarriers;
	span<VkImageMemoryBarrier> imgBarriers;

	span<Image*> images;
	span<Buffer*> buffers;

	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override;
	void displayInspector(Gui& gui) const override;
	Matcher doMatch(const BarrierCmdBase& rhs) const;
};

struct WaitEventsCmd : BarrierCmdBase {
	span<Event*> events;

	std::string nameDesc() const override { return "WaitEvents"; }
	Type type() const override { return Type::sync; }
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override;
	float match(const Command& rhs) const override;
};

struct BarrierCmd : BarrierCmdBase {
    VkDependencyFlags dependencyFlags {};

	std::string nameDesc() const override { return "PipelineBarrier"; }
	Type type() const override { return Type::sync; }
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
	float match(const Command& rhs) const override;
};

// All direct children must be of type 'NextSubpassCmd'
struct BeginRenderPassCmd : SectionCommand {
	VkRenderPassBeginInfo info {};
	span<VkClearValue> clearValues;

	Framebuffer* fb {};
	RenderPass* rp {};

	VkSubpassBeginInfo subpassBeginInfo; // for the first subpass

	// Returns the subpass that contains the given command.
	// Returns u32(-1) if no subpass is ancestor of the given command.
	u32 subpassOfDescendant(const Command& cmd) const;

	std::string toString() const override;
	std::vector<const Command*> display(const Command*, TypeFlags) const override;
	std::string nameDesc() const override { return "BeginRenderPass"; }
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override;
	float match(const Command& rhs) const override;
};

struct SubpassCmd : SectionCommand {};

struct NextSubpassCmd : SubpassCmd {
	VkSubpassEndInfo endInfo {}; // for the previous subpass
	VkSubpassBeginInfo beginInfo; // for the new subpass
	u32 subpassID {};

	using SubpassCmd::SubpassCmd;

	// toString should probably rather return the subpass number.
	// Must be tracked while recording
	std::string nameDesc() const override { return "NextSubpass"; }
	void record(const Device&, VkCommandBuffer) const override;
	float match(const Command& rhs) const override;
};

// Meta command needed for correct hierachy. We want each subpass to
// have its own section.
struct FirstSubpassCmd : SubpassCmd {
	using SubpassCmd::SubpassCmd;
	std::string nameDesc() const override { return "Subpass 0"; }
	void record(const Device&, VkCommandBuffer) const override {}
};

struct EndRenderPassCmd : Command {
	VkSubpassEndInfo endInfo {}; // for the previous subpass

	std::string nameDesc() const override { return "EndRenderPass"; }
	Type type() const override { return Type::end; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct DrawCmdBase : Command {
	GraphicsState state;
	PushConstantData pushConstants;

	DrawCmdBase() = default;
	DrawCmdBase(CommandBuffer& cb, const GraphicsState&);

	Type type() const override { return Type::draw; }
	void displayGrahpicsState(Gui& gui, bool indices) const;
	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override;
	Matcher doMatch(const DrawCmdBase& cmd, bool indexed) const;
};

struct DrawCmd : DrawCmdBase {
	u32 vertexCount;
	u32 instanceCount;
	u32 firstVertex;
	u32 firstInstance;

	using DrawCmdBase::DrawCmdBase;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "Draw"; }
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
	float match(const Command&) const override;
};

struct DrawIndirectCmd : DrawCmdBase {
	Buffer* buffer {};
	VkDeviceSize offset {};
	u32 drawCount {};
	u32 stride {};
	bool indexed {};

	using DrawCmdBase::DrawCmdBase;

	std::string toString() const override;
	std::string nameDesc() const override {
		return indexed ? "DrawIndexedIndirect" : "DrawIndirect";
	}
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override;
	float match(const Command&) const override;
};

struct DrawIndexedCmd : DrawCmdBase {
	u32 indexCount;
	u32 instanceCount;
	u32 firstIndex;
	i32 vertexOffset;
	u32 firstInstance;

	using DrawCmdBase::DrawCmdBase;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "DrawIndexed"; }
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
	float match(const Command&) const override;
};

struct DrawIndirectCountCmd : DrawCmdBase {
	Buffer* buffer {};
	VkDeviceSize offset {};
	u32 maxDrawCount {};
	u32 stride {};
	Buffer* countBuffer {};
	VkDeviceSize countBufferOffset {};
	bool indexed {};

	using DrawCmdBase::DrawCmdBase;

	std::string toString() const override;
	std::string nameDesc() const override {
		return indexed ? "DrawIndexedIndirectCount" : "DrawIndirectCount";
	}
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override;
	float match(const Command&) const override;
};

struct BindVertexBuffersCmd : Command {
	u32 firstBinding;
	span<BoundVertexBuffer> buffers;

	std::string toString() const override;
	void displayInspector(Gui&) const override;

	std::string nameDesc() const override { return "BindVertexBuffers"; }
	Type type() const override { return Type::bind; }
	void record(const Device&, VkCommandBuffer) const override;
	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override;
};

struct BindIndexBufferCmd : Command {
	Buffer* buffer {};
	VkDeviceSize offset {};
	VkIndexType indexType {};

	std::string nameDesc() const override { return "BindIndexBuffer"; }
	Type type() const override { return Type::bind; }
	void record(const Device&, VkCommandBuffer) const override;
	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override;
};

struct BindDescriptorSetCmd : Command {
	u32 firstSet;
	VkPipelineBindPoint pipeBindPoint;
	PipelineLayout* pipeLayout; // kept alive via shared_ptr in CommandBuffer
	span<DescriptorSet*> sets;
	span<u32> dynamicOffsets;

	std::string toString() const override;
	std::string nameDesc() const override { return "BindDescriptorSets"; }
	Type type() const override { return Type::bind; }
	void record(const Device&, VkCommandBuffer) const override;
	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override;
	void displayInspector(Gui& gui) const override;
};

struct DispatchCmdBase : Command {
	ComputeState state;
	PushConstantData pushConstants;

	DispatchCmdBase() = default;
	DispatchCmdBase(CommandBuffer&, const ComputeState&);

	Type type() const override { return Type::dispatch; }
	void displayComputeState(Gui& gui) const;
	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override;
	Matcher doMatch(const DispatchCmdBase&) const;
};

struct DispatchCmd : DispatchCmdBase {
	u32 groupsX {};
	u32 groupsY {};
	u32 groupsZ {};

	using DispatchCmdBase::DispatchCmdBase;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "Dispatch"; }
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
	float match(const Command&) const override;
};

struct DispatchIndirectCmd : DispatchCmdBase {
	Buffer* buffer {};
	VkDeviceSize offset {};

	using DispatchCmdBase::DispatchCmdBase;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "DispatchIndirect"; }
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override;
	float match(const Command&) const override;
};

struct DispatchBaseCmd : DispatchCmdBase {
	u32 baseGroupX {};
	u32 baseGroupY {};
	u32 baseGroupZ {};
	u32 groupsX {};
	u32 groupsY {};
	u32 groupsZ {};

	using DispatchCmdBase::DispatchCmdBase;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "DispatchBase"; }
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
	float match(const Command&) const override;
};

// transfer commands
struct CopyImageCmd : Command {
	Image* src {};
	Image* dst {};
	VkImageLayout srcLayout {};
	VkImageLayout dstLayout {};
	span<VkImageCopy2KHR> copies;
	const void* pNext {};

	std::string toString() const override;
	Type type() const override { return Type::transfer; }
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "CopyImage"; }
	std::vector<std::string> argumentsDesc() const override;
	void record(const Device&, VkCommandBuffer) const override;
	void unset(const std::unordered_set<DeviceHandle*>&) override;
};

struct CopyBufferToImageCmd : Command {
	Buffer* src {};
	Image* dst {};
	VkImageLayout dstLayout {};
	span<VkBufferImageCopy2KHR> copies;
	const void* pNext {};

	std::string toString() const override;
	Type type() const override { return Type::transfer; }
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "CopyBufferToImage"; }
	std::vector<std::string> argumentsDesc() const override;
	void record(const Device&, VkCommandBuffer) const override;
	void unset(const std::unordered_set<DeviceHandle*>&) override;
};

struct CopyImageToBufferCmd : Command {
	Image* src {};
	Buffer* dst {};
	VkImageLayout srcLayout {};
	span<VkBufferImageCopy2KHR> copies;
	const void* pNext {};

	std::string toString() const override;
	Type type() const override { return Type::transfer; }
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "CopyImageToBuffer"; }
	std::vector<std::string> argumentsDesc() const override;
	void record(const Device&, VkCommandBuffer) const override;
	void unset(const std::unordered_set<DeviceHandle*>&) override;
};

struct BlitImageCmd : Command {
	Image* src {};
	Image* dst {};
	VkImageLayout srcLayout {};
	VkImageLayout dstLayout {};
	span<VkImageBlit2KHR> blits;
	VkFilter filter {};
	const void* pNext {};

	std::string toString() const override;
	Type type() const override { return Type::transfer; }
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "BlitImage"; }
	std::vector<std::string> argumentsDesc() const override;
	void record(const Device&, VkCommandBuffer) const override;
	void unset(const std::unordered_set<DeviceHandle*>&) override;
};

struct ResolveImageCmd : Command {
	Image* src {};
	VkImageLayout srcLayout {};
	Image* dst {};
	VkImageLayout dstLayout {};
	span<VkImageResolve2KHR> regions;
	const void* pNext {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "ResolveImage"; }
	std::vector<std::string> argumentsDesc() const override;
	Type type() const override { return Type::transfer; }
	void record(const Device&, VkCommandBuffer) const override;
	void unset(const std::unordered_set<DeviceHandle*>&) override;
};

struct CopyBufferCmd : Command {
	Buffer* src {};
	Buffer* dst {};
	span<VkBufferCopy2KHR> regions;
	const void* pNext {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "CopyBuffer"; }
	std::vector<std::string> argumentsDesc() const override;
	Type type() const override { return Type::transfer; }
	void record(const Device&, VkCommandBuffer) const override;
	void unset(const std::unordered_set<DeviceHandle*>&) override;
};

struct UpdateBufferCmd : Command {
	Buffer* dst {};
	VkDeviceSize offset {};
	span<std::byte> data;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "UpdateBuffer"; }
	std::vector<std::string> argumentsDesc() const override;
	Type type() const override { return Type::transfer; }
	void record(const Device&, VkCommandBuffer) const override;
	void unset(const std::unordered_set<DeviceHandle*>&) override;
};

struct FillBufferCmd : Command {
	Buffer* dst {};
	VkDeviceSize offset {};
	VkDeviceSize size {};
	u32 data {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "FillBuffer"; }
	std::vector<std::string> argumentsDesc() const override;
	Type type() const override { return Type::transfer; }
	void record(const Device&, VkCommandBuffer) const override;
	void unset(const std::unordered_set<DeviceHandle*>&) override;
};

struct ClearColorImageCmd : Command {
	Image* dst {};
	VkClearColorValue color;
	VkImageLayout dstLayout {};
	span<VkImageSubresourceRange> ranges;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	Type type() const override { return Type::transfer; }
	std::string nameDesc() const override { return "ClearColorImage"; }
	std::vector<std::string> argumentsDesc() const override;
	void record(const Device&, VkCommandBuffer) const override;
	void unset(const std::unordered_set<DeviceHandle*>&) override;
};

struct ClearDepthStencilImageCmd : Command {
	Image* dst {};
	VkClearDepthStencilValue value {};
	VkImageLayout dstLayout {};
	span<VkImageSubresourceRange> ranges;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	Type type() const override { return Type::transfer; }
	std::string nameDesc() const override { return "ClearDepthStencilImage"; }
	std::vector<std::string> argumentsDesc() const override;
	void record(const Device&, VkCommandBuffer) const override;
	void unset(const std::unordered_set<DeviceHandle*>&) override;
};

struct ClearAttachmentCmd : Command {
	span<VkClearAttachment> attachments;
	span<VkClearRect> rects;
	RenderPassInstanceState rpi;

	std::string nameDesc() const override { return "ClearAttachment"; }
	std::vector<std::string> argumentsDesc() const override;
	void displayInspector(Gui& gui) const override;
	Type type() const override { return Type::transfer; }
	void record(const Device&, VkCommandBuffer) const override;
	void unset(const std::unordered_set<DeviceHandle*>&) override;
};

struct SetEventCmd : Command {
	Event* event {};
	VkPipelineStageFlags stageMask {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "SetEvent"; }
	std::vector<std::string> argumentsDesc() const override;
	Type type() const override { return Type::sync; }
	void record(const Device&, VkCommandBuffer) const override;
	void unset(const std::unordered_set<DeviceHandle*>&) override;
};

struct ResetEventCmd : Command {
	Event* event {};
	VkPipelineStageFlags stageMask {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "ResetEvent"; }
	std::vector<std::string> argumentsDesc() const override;
	Type type() const override { return Type::sync; }
	void record(const Device&, VkCommandBuffer) const override;
	void unset(const std::unordered_set<DeviceHandle*>&) override;
};

struct ExecuteCommandsCmd : ParentCommand {
	Command* children_ {};

	Command* children() const override { return children_; }
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "ExecuteCommands"; }
	std::vector<const Command*> display(const Command*, TypeFlags) const override;
	void record(const Device&, VkCommandBuffer) const override;
};

// Meta-command
struct ExecuteCommandsChildCmd : ParentCommand {
	CommandRecord* record_ {}; // kept alive in parent CommandRecord
	unsigned id_ {};

	std::string nameDesc() const override { return "ExecuteCommandsChild"; }
	std::string toString() const override;
	Command* children() const override { return record_->commands; }
	void record(const Device&, VkCommandBuffer) const override {}
};

struct BeginDebugUtilsLabelCmd : SectionCommand {
	const char* name {};
	std::array<float, 4> color; // NOTE: could use this in UI

	std::string toString() const override { return dlg::format("Label: {}", name); }
	void record(const Device&, VkCommandBuffer) const override;

	// NOTE: yes, we return more than just the command here.
	// But that's because the command itself isn't of any use without the label.
	std::string nameDesc() const override { return toString(); }
};

struct EndDebugUtilsLabelCmd : Command {
	std::string nameDesc() const override { return dlg::format("EndLabel"); }
	Type type() const override { return Type::end; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct BindPipelineCmd : Command {
	VkPipelineBindPoint bindPoint {};
	Pipeline* pipe {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "BindPipeline"; }
	Type type() const override { return Type::bind; }
	void record(const Device&, VkCommandBuffer) const override;
	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override;
};

struct PushConstantsCmd : Command {
	PipelineLayout* pipeLayout; // kept alive via shared_ptr in CommandBuffer
	VkShaderStageFlags stages {};
	u32 offset {};
	span<std::byte> values;

	std::string nameDesc() const override { return "PushConstants"; }
	Type type() const override { return Type::bind; }
	void record(const Device&, VkCommandBuffer) const override;
};

// dynamic state
struct SetViewportCmd : Command {
	u32 first {};
	span<VkViewport> viewports;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetViewport"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetScissorCmd : Command {
	u32 first {};
	span<VkRect2D> scissors;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetScissor"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetLineWidthCmd : Command {
	float width {};

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetLineWidth"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetDepthBiasCmd : Command {
	DynamicStateDepthBias state {};

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetDepthBias"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetDepthBoundsCmd : Command {
	float min {};
	float max {};

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetDepthBounds"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetBlendConstantsCmd : Command {
	std::array<float, 4> values {};

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetBlendConstants"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetStencilCompareMaskCmd : Command {
	VkStencilFaceFlags faceMask {};
	u32 value {};

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetStencilCompareMask"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetStencilWriteMaskCmd : Command {
	VkStencilFaceFlags faceMask {};
	u32 value {};

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetStencilWriteMask"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetStencilReferenceCmd : Command {
	VkStencilFaceFlags faceMask {};
	u32 value {};

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetStencilReference"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

// query pool
struct BeginQueryCmd : Command {
	QueryPool* pool {};
	u32 query {};
	VkQueryControlFlags flags {};

	Type type() const override { return Type::query; }
	std::string toString() const override { return "BeginQuery"; }
	std::string nameDesc() const override { return "BeginQuery"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override;
};

struct EndQueryCmd : Command {
	QueryPool* pool {};
	u32 query {};

	Type type() const override { return Type::query; }
	std::string toString() const override { return "EndQuery"; }
	std::string nameDesc() const override { return "EndQuery"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override;
};

struct ResetQueryPoolCmd : Command {
	QueryPool* pool {};
	u32 first {};
	u32 count {};

	Type type() const override { return Type::query; }
	std::string toString() const override { return "ResetQueryPool"; }
	std::string nameDesc() const override { return "ResetQueryPool"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override;
};

struct WriteTimestampCmd : Command {
	QueryPool* pool {};
	VkPipelineStageFlagBits stage {};
	u32 query {};

	Type type() const override { return Type::query; }
	std::string toString() const override { return "WriteTimestamp"; }
	std::string nameDesc() const override { return "WriteTimestamp"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override;
};

struct CopyQueryPoolResultsCmd : Command {
	QueryPool* pool {};
	u32 first {};
	u32 count {};
	Buffer* dstBuffer {};
	VkDeviceSize dstOffset {};
	VkDeviceSize stride {};
	VkQueryResultFlags flags {};

	Type type() const override { return Type::query; }
	std::string toString() const override { return "CopyQueryPoolResults"; }
	std::string nameDesc() const override { return "CopyQueryPoolResults"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void unset(const std::unordered_set<DeviceHandle*>& destroyed) override;
};

struct PushDescriptorSetCmd : Command {
	VkPipelineBindPoint bindPoint {};
	PipelineLayout* pipeLayout {};
	u32 set {};

	// The individual pImageInfo, pBufferInfo, pTexelBufferView arrays
	// are allocated in the CommandRecord-owned memory as well.
	span<VkWriteDescriptorSet> descriptorWrites;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "PushDescriptorSet"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct PushDescriptorSetWithTemplateCmd : Command {
	DescriptorUpdateTemplate* updateTemplate {};
	PipelineLayout* pipeLayout {};
	u32 set {};
	span<const std::byte> data;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "PushDescriptorSetWithTemplate"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

} // namespace vil

#ifdef __GNUC__
	#pragma GCC diagnostic pop
#endif // __GNUC__
