#pragma once

#include <command/record.hpp>
#include <pipe.hpp>
#include <util/flags.hpp>
#include <util/span.hpp>
#include <dlg/dlg.hpp>

// See ~Command. Keep in mind that we use a custom per-CommandRecord allocator
// for all Commands, that's why we can use span<> here without the referenced
// data being owned anywhere. It's just guaranteed to stay alive as long
// as the CommandRecord.
#ifdef __GNUC__
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif // __GNUC__

namespace backward {

class StackTrace;

} // namespace backward

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
	// TracyRays commands
	traceRays = (1u << 8u),
	// BuildAccelerationStructures commands
	buildAccelStruct = (1u << 9u),
};

// Represents the result of a matching operation.
// The effective matching value is 'match/total' but having both values
// gives additional information for further processing.
// Invariant: For common objects, match <= total.
struct Matcher {
	float match {}; // sum of weights of comparisons that matched
	float total {}; // sum of weights of all comparisons

	// Special constant to signal that matching can't work and should
	// be aborted.
	static Matcher noMatch() { return {0.f, -1.f}; }
};

float eval(const Matcher& m);

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
	// be separately stored in the command record.
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

	// TODO: can and should be removed, eventually
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

	// Should replace all internal handles that are among the keys of
	// the given map with the values they are mapped to.
	// Should forward the call to all potential children.
	// In practice, this is used mostly to inform Commands about destroyed
	// handles.
	virtual void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>&) {}

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
	virtual Matcher match(const Command& cmd) const;

	// Forms a forward linked list with siblings
	Command* next {};

	// How many sibilings with same nameDesc() came before this in parent
	// NOTE: i don't like this here. Not sure how to properly solve this.
	// It's needed by displayCommands to generate proper imgui ids
	unsigned relID {};

	backward::StackTrace* stackTrace {};
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
		const Command* selected, Command::TypeFlags typeFlags, bool firstSep);

struct ParentCommand : Command {
	std::vector<const Command*> display(const Command* selected,
		TypeFlags typeFlags, const Command* cmd) const;
	std::vector<const Command*> display(const Command* selected,
		TypeFlags typeFlags) const override;

	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override {
		auto* cmd = children();
		while(cmd) {
			cmd->replace(map);
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

	// NOTE: a bit hacky to store this information here but we need
	// it to know whether family transitions are release or acquire.
	// Should probably rather be passed to the record command
	u32 recordQueueFamilyIndex {};

	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	void displayInspector(Gui& gui) const override;
	Matcher doMatch(const BarrierCmdBase& rhs) const;

	struct PatchedBarriers {
		span<VkMemoryBarrier> memBarriers;
		span<VkBufferMemoryBarrier> bufBarriers;
		span<VkImageMemoryBarrier> imgBarriers;
	};

	PatchedBarriers patchedBarriers(ThreadMemScope& memScope) const;
};

struct WaitEventsCmd : BarrierCmdBase {
	span<Event*> events;

	std::string nameDesc() const override { return "WaitEvents"; }
	Type type() const override { return Type::sync; }
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command& rhs) const override;
};

struct BarrierCmd : BarrierCmdBase {
    VkDependencyFlags dependencyFlags {};

	std::string nameDesc() const override { return "PipelineBarrier"; }
	Type type() const override { return Type::sync; }
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer) const override;
	Matcher match(const Command& rhs) const override;
};

// All direct children must be of type 'NextSubpassCmd'
struct BeginRenderPassCmd : SectionCommand {
	VkRenderPassBeginInfo info {};
	span<VkClearValue> clearValues;

	Framebuffer* fb {};
	RenderPass* rp {};

	// Used attachments, stored here separately from framebuffer since
	// the framebuffer mgiht be imageless
	span<ImageView*> attachments;

	VkSubpassBeginInfo subpassBeginInfo; // for the first subpass

	// Returns the subpass that contains the given command.
	// Returns u32(-1) if no subpass is ancestor of the given command.
	u32 subpassOfDescendant(const Command& cmd) const;

	std::string toString() const override;
	std::vector<const Command*> display(const Command*, TypeFlags) const override;
	std::string nameDesc() const override { return "BeginRenderPass"; }
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command& rhs) const override;
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
	Matcher match(const Command& rhs) const override;
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

// Base command from which all draw, dispatch and traceRays command are derived.
struct StateCmdBase : Command {
	virtual const DescriptorState& boundDescriptors() const = 0;
	virtual const Pipeline* boundPipe() const = 0;
	virtual const PushConstantData& boundPushConstants() const = 0;
};

struct DrawCmdBase : StateCmdBase {
	GraphicsState state;
	PushConstantData pushConstants;

	DrawCmdBase(CommandBuffer& cb, const GraphicsState&);

	Type type() const override { return Type::draw; }
	void displayGrahpicsState(Gui& gui, bool indices) const;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher doMatch(const DrawCmdBase& cmd, bool indexed) const;

	const DescriptorState& boundDescriptors() const override { return state; }
	const Pipeline* boundPipe() const override { return state.pipe; }
	const PushConstantData& boundPushConstants() const override { return pushConstants; }
};

struct DrawCmd final : DrawCmdBase {
	u32 vertexCount;
	u32 instanceCount;
	u32 firstVertex;
	u32 firstInstance;

	using DrawCmdBase::DrawCmdBase;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "Draw"; }
	void record(const Device&, VkCommandBuffer) const override;
	Matcher match(const Command&) const override;
};

struct DrawIndirectCmd final : DrawCmdBase {
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
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command&) const override;
};

struct DrawIndexedCmd final : DrawCmdBase {
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
	Matcher match(const Command&) const override;
};

struct DrawIndirectCountCmd final : DrawCmdBase {
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
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command&) const override;
};

struct BindVertexBuffersCmd final : Command {
	u32 firstBinding;
	span<BoundVertexBuffer> buffers;

	std::string toString() const override;
	void displayInspector(Gui&) const override;

	std::string nameDesc() const override { return "BindVertexBuffers"; }
	Type type() const override { return Type::bind; }
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command&) const override;
};

struct BindIndexBufferCmd final : Command {
	Buffer* buffer {};
	VkDeviceSize offset {};
	VkIndexType indexType {};

	std::string nameDesc() const override { return "BindIndexBuffer"; }
	Type type() const override { return Type::bind; }
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command&) const override;
};

struct BindDescriptorSetCmd final : Command {
	u32 firstSet;
	VkPipelineBindPoint pipeBindPoint;
	PipelineLayout* pipeLayout; // kept alive via shared_ptr in CommandBuffer
	span<DescriptorSet*> sets; // NOTE: handles may be invalid
	span<u32> dynamicOffsets;

	std::string toString() const override;
	std::string nameDesc() const override { return "BindDescriptorSets"; }
	Type type() const override { return Type::bind; }
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	void displayInspector(Gui& gui) const override;
	Matcher match(const Command&) const override;
};

struct DispatchCmdBase : StateCmdBase {
	ComputeState state;
	PushConstantData pushConstants;

	DispatchCmdBase(CommandBuffer&, const ComputeState&);

	Type type() const override { return Type::dispatch; }
	void displayComputeState(Gui& gui) const;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher doMatch(const DispatchCmdBase&) const;

	const DescriptorState& boundDescriptors() const override { return state; }
	const Pipeline* boundPipe() const override { return state.pipe; }
	const PushConstantData& boundPushConstants() const override { return pushConstants; }
};

struct DispatchCmd final : DispatchCmdBase {
	u32 groupsX {};
	u32 groupsY {};
	u32 groupsZ {};

	using DispatchCmdBase::DispatchCmdBase;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "Dispatch"; }
	void record(const Device&, VkCommandBuffer) const override;
	Matcher match(const Command&) const override;
};

struct DispatchIndirectCmd final : DispatchCmdBase {
	Buffer* buffer {};
	VkDeviceSize offset {};

	using DispatchCmdBase::DispatchCmdBase;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "DispatchIndirect"; }
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command&) const override;
};

struct DispatchBaseCmd final : DispatchCmdBase {
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
	Matcher match(const Command&) const override;
};

// transfer commands
struct CopyImageCmd final : Command {
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
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command&) const override;
};

struct CopyBufferToImageCmd final : Command {
	Buffer* src {};
	Image* dst {};
	VkImageLayout dstLayout {};
	span<VkBufferImageCopy2KHR> copies;
	const void* pNext {};

	std::string toString() const override;
	Type type() const override { return Type::transfer; }
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "CopyBufferToImage"; }
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command&) const override;
};

struct CopyImageToBufferCmd final : Command {
	Image* src {};
	Buffer* dst {};
	VkImageLayout srcLayout {};
	span<VkBufferImageCopy2KHR> copies;
	const void* pNext {};

	std::string toString() const override;
	Type type() const override { return Type::transfer; }
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "CopyImageToBuffer"; }
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command&) const override;
};

struct BlitImageCmd final : Command {
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
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command&) const override;
};

struct ResolveImageCmd final : Command {
	Image* src {};
	VkImageLayout srcLayout {};
	Image* dst {};
	VkImageLayout dstLayout {};
	span<VkImageResolve2KHR> regions;
	const void* pNext {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "ResolveImage"; }
	Type type() const override { return Type::transfer; }
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command&) const override;
};

struct CopyBufferCmd final : Command {
	Buffer* src {};
	Buffer* dst {};
	span<VkBufferCopy2KHR> regions;
	const void* pNext {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "CopyBuffer"; }
	Type type() const override { return Type::transfer; }
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command&) const override;
};

struct UpdateBufferCmd final : Command {
	Buffer* dst {};
	VkDeviceSize offset {};
	span<std::byte> data;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "UpdateBuffer"; }
	Type type() const override { return Type::transfer; }
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command&) const override;
};

struct FillBufferCmd final : Command {
	Buffer* dst {};
	VkDeviceSize offset {};
	VkDeviceSize size {};
	u32 data {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "FillBuffer"; }
	Type type() const override { return Type::transfer; }
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command&) const override;
};

struct ClearColorImageCmd final : Command {
	Image* dst {};
	VkClearColorValue color;
	VkImageLayout dstLayout {};
	span<VkImageSubresourceRange> ranges;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	Type type() const override { return Type::transfer; }
	std::string nameDesc() const override { return "ClearColorImage"; }
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command&) const override;
};

struct ClearDepthStencilImageCmd final : Command {
	Image* dst {};
	VkClearDepthStencilValue value {};
	VkImageLayout dstLayout {};
	span<VkImageSubresourceRange> ranges;

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	Type type() const override { return Type::transfer; }
	std::string nameDesc() const override { return "ClearDepthStencilImage"; }
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command&) const override;
};

struct ClearAttachmentCmd final : Command {
	span<VkClearAttachment> attachments;
	span<VkClearRect> rects;
	RenderPassInstanceState rpi;

	std::string nameDesc() const override { return "ClearAttachment"; }
	void displayInspector(Gui& gui) const override;
	Type type() const override { return Type::transfer; }
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command&) const override;
};

struct SetEventCmd final : Command {
	Event* event {};
	VkPipelineStageFlags stageMask {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "SetEvent"; }
	Type type() const override { return Type::sync; }
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
};

struct ResetEventCmd final : Command {
	Event* event {};
	VkPipelineStageFlags stageMask {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "ResetEvent"; }
	Type type() const override { return Type::sync; }
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
};

struct ExecuteCommandsCmd final : ParentCommand {
	Command* children_ {};

	Command* children() const override { return children_; }
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "ExecuteCommands"; }
	std::vector<const Command*> display(const Command*, TypeFlags) const override;
	void record(const Device&, VkCommandBuffer) const override;
};

// Meta-command
struct ExecuteCommandsChildCmd final : ParentCommand {
	CommandRecord* record_ {}; // kept alive in parent CommandRecord
	unsigned id_ {};

	std::string nameDesc() const override { return "ExecuteCommandsChild"; }
	std::string toString() const override;
	Command* children() const override { return record_->commands; }
	void record(const Device&, VkCommandBuffer) const override {}
};

struct BeginDebugUtilsLabelCmd final : SectionCommand {
	const char* name {};
	std::array<float, 4> color; // NOTE: could use this in UI

	std::string toString() const override { return dlg::format("Label: {}", name); }
	void record(const Device&, VkCommandBuffer) const override;
	Matcher match(const Command&) const override;

	// NOTE: yes, we return more than just the command here.
	// But that's because the command itself isn't of any use without the label.
	std::string nameDesc() const override { return toString(); }
};

struct EndDebugUtilsLabelCmd final : Command {
	std::string nameDesc() const override { return dlg::format("EndLabel"); }
	Type type() const override { return Type::end; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct BindPipelineCmd final : Command {
	VkPipelineBindPoint bindPoint {};
	Pipeline* pipe {};

	std::string toString() const override;
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "BindPipeline"; }
	Type type() const override { return Type::bind; }
	void record(const Device&, VkCommandBuffer) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
	Matcher match(const Command&) const override;
};

struct PushConstantsCmd final : Command {
	PipelineLayout* pipeLayout; // kept alive via shared_ptr in CommandBuffer
	VkShaderStageFlags stages {};
	u32 offset {};
	span<std::byte> values;

	std::string nameDesc() const override { return "PushConstants"; }
	Type type() const override { return Type::bind; }
	void record(const Device&, VkCommandBuffer) const override;
	Matcher match(const Command&) const override;
};

// dynamic state
struct SetViewportCmd final : Command {
	u32 first {};
	span<VkViewport> viewports;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetViewport"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void displayInspector(Gui& gui) const override;
};

struct SetScissorCmd final : Command {
	u32 first {};
	span<VkRect2D> scissors;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetScissor"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void displayInspector(Gui& gui) const override;
};

struct SetLineWidthCmd final : Command {
	float width {};

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetLineWidth"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void displayInspector(Gui& gui) const override;
};

struct SetDepthBiasCmd final : Command {
	DynamicStateDepthBias state {};

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetDepthBias"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetDepthBoundsCmd final : Command {
	float min {};
	float max {};

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetDepthBounds"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetBlendConstantsCmd final : Command {
	std::array<float, 4> values {};

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetBlendConstants"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetStencilCompareMaskCmd final : Command {
	VkStencilFaceFlags faceMask {};
	u32 value {};

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetStencilCompareMask"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetStencilWriteMaskCmd final : Command {
	VkStencilFaceFlags faceMask {};
	u32 value {};

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetStencilWriteMask"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetStencilReferenceCmd final : Command {
	VkStencilFaceFlags faceMask {};
	u32 value {};

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetStencilReference"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

// query pool
struct BeginQueryCmd final : Command {
	QueryPool* pool {};
	u32 query {};
	VkQueryControlFlags flags {};

	Type type() const override { return Type::query; }
	std::string nameDesc() const override { return "BeginQuery"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
};

struct EndQueryCmd final : Command {
	QueryPool* pool {};
	u32 query {};

	Type type() const override { return Type::query; }
	std::string nameDesc() const override { return "EndQuery"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
};

struct ResetQueryPoolCmd final : Command {
	QueryPool* pool {};
	u32 first {};
	u32 count {};

	Type type() const override { return Type::query; }
	std::string nameDesc() const override { return "ResetQueryPool"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
};

struct WriteTimestampCmd final : Command {
	QueryPool* pool {};
	VkPipelineStageFlagBits stage {};
	u32 query {};

	Type type() const override { return Type::query; }
	std::string nameDesc() const override { return "WriteTimestamp"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
};

struct CopyQueryPoolResultsCmd final : Command {
	QueryPool* pool {};
	u32 first {};
	u32 count {};
	Buffer* dstBuffer {};
	VkDeviceSize dstOffset {};
	VkDeviceSize stride {};
	VkQueryResultFlags flags {};

	Type type() const override { return Type::query; }
	std::string nameDesc() const override { return "CopyQueryPoolResults"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
};

// Push descriptor commands
struct PushDescriptorSetCmd final : Command {
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

struct PushDescriptorSetWithTemplateCmd final : Command {
	DescriptorUpdateTemplate* updateTemplate {};
	PipelineLayout* pipeLayout {};
	u32 set {};
	span<const std::byte> data;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "PushDescriptorSetWithTemplate"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

// VK_KHR_fragment_shading_rate
struct SetFragmentShadingRateCmd final : Command {
	VkExtent2D fragmentSize;
	std::array<VkFragmentShadingRateCombinerOpKHR, 2> combinerOps;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetFragmentShadingRateCmd"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

// VK_EXT_conditional_rendering
struct BeginConditionalRenderingCmd final : SectionCommand {
	Buffer* buffer;
	VkDeviceSize offset;
	VkConditionalRenderingFlagsEXT flags;

	Type type() const override { return Type::other; }
	std::string nameDesc() const override { return "BeginConditionalRendering"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
};

struct EndConditionalRenderingCmd final : Command {
	Type type() const override { return Type::end; }
	std::string nameDesc() const override { return "EndConditionalRendering"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

// VK_EXT_line_rasterization
struct SetLineStippleCmd final : Command {
	u32 stippleFactor;
	u16 stipplePattern;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetLineStipple"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

// VK_EXT_extended_dynamic_state
struct SetCullModeCmd final : Command {
	VkCullModeFlags cullMode;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetCullMode"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetFrontFaceCmd final : Command {
	VkFrontFace frontFace;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetFrontFace"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetPrimitiveTopologyCmd final : Command {
	VkPrimitiveTopology topology;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetPrimitiveTopology"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetViewportWithCountCmd final : Command {
	span<VkViewport> viewports;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetViewportWithCount"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetScissorWithCountCmd final : Command {
	span<VkRect2D> scissors;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetScissorWithCount"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

// BindVertexBuffers2Cmd should be mapped via BindVertexBuffers

struct SetDepthTestEnableCmd final : Command {
	bool enable;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetDepthTestEnable"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetDepthWriteEnableCmd final : Command {
	bool enable;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetDepthWriteEnable"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetDepthCompareOpCmd final : Command {
	VkCompareOp op;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetDepthCompareOp"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetDepthBoundsTestEnableCmd final : Command {
	bool enable;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetDepthBoundsTestEnable"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetStencilTestEnableCmd final : Command {
	bool enable;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetStencilTestEnable"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct SetStencilOpCmd final : Command {
    VkStencilFaceFlags faceMask;
    VkStencilOp failOp;
    VkStencilOp passOp;
    VkStencilOp depthFailOp;
    VkCompareOp compareOp;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetStencilOp"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

// VK_EXT_sample_locations
struct SetSampleLocationsCmd final : Command {
	VkSampleLocationsInfoEXT info;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetSampleLocations"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

// VK_EXT_discard_rectangles
struct SetDiscardRectangleCmd final : Command {
	u32 first;
	span<VkRect2D> rects;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetDiscardRectangle"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

// VK_KHR_acceleration_structure
struct CopyAccelStructureCmd final : Command {
	const void* pNext {};
	AccelStruct* src {};
	AccelStruct* dst {};
    VkCopyAccelerationStructureModeKHR mode;

	Type type() const override { return Type::transfer; }
	std::string nameDesc() const override { return "CopyAccelerationStructure"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
};

struct CopyAccelStructToMemoryCmd final : Command {
	const void* pNext {};
	AccelStruct* src {};
    VkDeviceOrHostAddressKHR dst {};
    VkCopyAccelerationStructureModeKHR mode;

	Type type() const override { return Type::transfer; }
	std::string nameDesc() const override { return "CopyAccelerationStructureToMemory"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
};

struct CopyMemoryToAccelStructCmd final : Command {
	const void* pNext {};
    VkDeviceOrHostAddressConstKHR src {};
	AccelStruct* dst {};
    VkCopyAccelerationStructureModeKHR mode;

	Type type() const override { return Type::transfer; }
	std::string nameDesc() const override { return "CopyMemoryToAccelerationStructure"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
};

struct WriteAccelStructsPropertiesCmd final : Command {
	span<AccelStruct*> accelStructs;
	VkQueryType queryType {};
	QueryPool* queryPool {};
	u32 firstQuery {};

	Type type() const override { return Type::query; }
	std::string nameDesc() const override { return "WriteAccelerationStructuresProperties"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
};

struct BuildAccelStructsCmd final : Command {
	span<VkAccelerationStructureBuildGeometryInfoKHR> buildInfos; // already handle-fwd-patched
	span<span<VkAccelerationStructureBuildRangeInfoKHR>> buildRangeInfos;
	span<AccelStruct*> srcs;
	span<AccelStruct*> dsts;

	BuildAccelStructsCmd(CommandBuffer& cb);
	Type type() const override { return Type::buildAccelStruct; }
	std::string nameDesc() const override { return "BuildAccelerationStructures"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
};

struct BuildAccelStructsIndirectCmd final : Command {
	span<VkAccelerationStructureBuildGeometryInfoKHR> buildInfos; // already handle-fwd-patched
	span<AccelStruct*> srcs;
	span<AccelStruct*> dsts;

	// indirect info. All span have a size equal to buildInfos.size()
	span<VkDeviceAddress> indirectAddresses;
	span<u32> indirectStrides;
	span<u32*> maxPrimitiveCounts;

	BuildAccelStructsIndirectCmd(CommandBuffer& cb);
	Type type() const override { return Type::buildAccelStruct; }
	std::string nameDesc() const override { return "BuildAccelerationStructuresIndirect"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
};

// VK_KHR_ray_tracing_pipeline
struct TraceRaysCmdBase : StateCmdBase {
	RayTracingState state;
	PushConstantData pushConstants;

	TraceRaysCmdBase(CommandBuffer& cb, const RayTracingState&);
	Type type() const override { return Type::traceRays; }
	Matcher doMatch(const TraceRaysCmdBase& cmd) const;

	const DescriptorState& boundDescriptors() const override { return state; }
	const Pipeline* boundPipe() const override { return state.pipe; }
	const PushConstantData& boundPushConstants() const override { return pushConstants; }
	void replace(const CommandAllocHashMap<DeviceHandle*, DeviceHandle*>& map) override;
};

struct TraceRaysCmd final : TraceRaysCmdBase {
	u32 width;
	u32 height;
	u32 depth;

	VkStridedDeviceAddressRegionKHR raygenBindingTable;
	VkStridedDeviceAddressRegionKHR missBindingTable;
	VkStridedDeviceAddressRegionKHR hitBindingTable;
	VkStridedDeviceAddressRegionKHR callableBindingTable;

	using TraceRaysCmdBase::TraceRaysCmdBase;
	std::string nameDesc() const override { return "TraceRays"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	Matcher match(const Command&) const override;
};

struct TraceRaysIndirectCmd final : TraceRaysCmdBase {
	VkDeviceAddress indirectDeviceAddress;
	VkStridedDeviceAddressRegionKHR raygenBindingTable;
	VkStridedDeviceAddressRegionKHR missBindingTable;
	VkStridedDeviceAddressRegionKHR hitBindingTable;
	VkStridedDeviceAddressRegionKHR callableBindingTable;

	using TraceRaysCmdBase::TraceRaysCmdBase;
	std::string nameDesc() const override { return "TraceRaysIndirect"; }
	void record(const Device&, VkCommandBuffer cb) const override;
	Matcher match(const Command&) const override;
};

struct SetRayTracingPipelineStackSizeCmd final : Command {
	u32 stackSize;

	Type type() const override { return Type::bind; }
	std::string nameDesc() const override { return "SetRayTracingPipelineStackSize"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

} // namespace vil

#ifdef __GNUC__
	#pragma GCC diagnostic pop
#endif // __GNUC__
