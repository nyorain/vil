#pragma once

#include <boundState.hpp>
#include <handles.hpp>
#include <flags.hpp>
#include <pv.hpp>
#include <imguiutil.hpp>
#include <span.hpp>
#include <dlg/dlg.hpp>

// See ~Command
#ifdef __GNUC__
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif // __GNUC__

namespace fuen {

// The list of commands in a CommandBuffer is organized as a tree.
// Section-like commands (e.g. cmdBeginRenderPass) have all their commands
// stored as children
struct Command {
	// The type of a command is used e.g. to hide them in the UI.
	enum class Type : u32 {
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

	using TypeFlags = nytl::Flags<Type>;

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
	// itself or one of its children, if selected, otherwise nullptr.
	virtual const Command* display(const Command* sel, TypeFlags typeFlags) const {
		if(!(typeFlags & this->type())) {
			return nullptr;
		}

		// ImGui::PushID(dlg::format("{}:{}", nameDesc(), relID).c_str());
		// ImGui::PushID(nameDesc().c_str());
		// ImGui::Bullet();
		// auto selected = (sel == this);
		// auto ret = ImGui::Selectable(toString().c_str(), selected) ? this : nullptr;
		// ImGui::PopID();

		int flags = ImGuiTreeNodeFlags_Leaf;
		if(sel == this) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		auto idStr = dlg::format("{}:{}", nameDesc(), relID);
		ImGui::TreeNodeEx(idStr.c_str(), flags, "%s", toString().c_str());

		const Command* ret = nullptr;
		if(ImGui::IsItemClicked()) {
			ret = this;
		}

		ImGui::TreePop();

		return ret;
	}

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
	virtual void displayInspector(Gui&) const {}

	// Might be null for toplevel commands
	SectionCommand* parent {};

	// Forms a linked list with siblings
	Command* next {};

	// How many sibilings with same nameDesc() came before this in parent
	unsigned relID {};
};

NYTL_FLAG_OPS(Command::Type)

const Command* displayCommands(const Command* cmd, const Command* selected,
		Command::TypeFlags typeFlags);

struct SectionCommand : Command {
	Command* children {};
	const Command* display(const Command* selected, TypeFlags typeFlags, const Command* cmd) const;
	const Command* display(const Command* selected, TypeFlags typeFlags) const override;
};

struct BarrierCmdBase : Command {
    VkPipelineStageFlags srcStageMask;
    VkPipelineStageFlags dstStageMask;

	span<VkMemoryBarrier> memBarriers;
	span<VkBufferMemoryBarrier> bufBarriers;
	span<VkImageMemoryBarrier> imgBarriers;

	span<Image*> images;
	span<Buffer*> buffers;
};

struct WaitEventsCmd : BarrierCmdBase {
	span<Event*> events;

	std::string nameDesc() const override { return "WaitEvents"; }
	Type type() const override { return Type::sync; }
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
};

struct BarrierCmd : BarrierCmdBase {
    VkDependencyFlags dependencyFlags;

	std::string nameDesc() const override { return "PipelineBarrier"; }
	Type type() const override { return Type::sync; }
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
};

// All direct children must be of type 'NextSubpassCmd'
struct BeginRenderPassCmd : SectionCommand {
	VkRenderPassBeginInfo info;
	span<VkClearValue> clearValues;

	Framebuffer* fb;
	RenderPass* rp;

	VkSubpassBeginInfo subpassBeginInfo; // for the first subpass

	using SectionCommand::SectionCommand;

	std::string toString() const override {
		return dlg::format("BeginRenderPass({}, {})", name(*fb), name(*rp));
	}

	const Command* display(const Command* selected, TypeFlags typeFlags) const override;
	std::string nameDesc() const override { return "BeginRenderPass"; }
	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
};

struct SubpassCmd : SectionCommand {
	using SectionCommand::SectionCommand;
};

struct NextSubpassCmd : SubpassCmd {
	VkSubpassEndInfo endInfo {}; // for the previous subpass
	VkSubpassBeginInfo beginInfo; // for the new subpass

	using SubpassCmd::SubpassCmd;

	// toString should probably rather return the subpass number.
	// Must be tracked while recording
	std::string nameDesc() const override { return "NextSubpass"; }
	void record(const Device&, VkCommandBuffer) const override;
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
};

struct DrawCmd : DrawCmdBase {
	u32 vertexCount;
	u32 instanceCount;
	u32 firstVertex;
	u32 firstInstance;

	using DrawCmdBase::DrawCmdBase;

	std::string toString() const override {
		return dlg::format("Draw({}, {}, {}, {})",
			vertexCount, instanceCount, firstVertex, firstInstance);
	}

	void displayInspector(Gui& gui) const override {
		asColumns2({{
			{"vertexCount", "{}", vertexCount},
			{"instanceCount", "{}", instanceCount},
			{"firstVertex", "{}", firstVertex},
			{"firstInstance", "{}", firstInstance},
		}});

		DrawCmdBase::displayGrahpicsState(gui, false);
	}

	std::string nameDesc() const override { return "Draw"; }
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
};

struct DrawIndirectCmd : DrawCmdBase {
	Buffer* buffer {};
	VkDeviceSize offset {};
	u32 drawCount {};
	u32 stride {};
	bool indexed {};

	using DrawCmdBase::DrawCmdBase;

	std::string toString() const override {
		return indexed ? "DrawIndexedIndirect" : "DrawIndirect";
	}
	std::string nameDesc() const override {
		return indexed ? "DrawIndexedIndirect" : "DrawIndirect";
	}

	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
};

struct DrawIndexedCmd : DrawCmdBase {
	u32 indexCount;
	u32 instanceCount;
	u32 firstIndex;
	i32 vertexOffset;
	u32 firstInstance;

	using DrawCmdBase::DrawCmdBase;

	std::string toString() const override {
		return dlg::format("DrawIndexed({}, {}, {}, {}, {})",
			indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
	}

	void displayInspector(Gui& gui) const override {
		asColumns2({{
			{"indexCount", "{}", indexCount},
			{"instanceCount", "{}", instanceCount},
			{"firstIndex", "{}", firstIndex},
			{"vertexOffset", "{}", vertexOffset},
			{"firstInstance", "{}", firstInstance},
		}});

		DrawCmdBase::displayGrahpicsState(gui, true);
	}

	std::string nameDesc() const override { return "DrawIndexed"; }
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
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

	std::string toString() const override {
		return indexed ? "DrawIndexedIndirectCount" : "DrawIndirectCount";
	}
	std::string nameDesc() const override {
		return indexed ? "DrawIndexedIndirectCount" : "DrawIndirectCount";
	}

	void displayInspector(Gui& gui) const override;
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
};

struct BindVertexBuffersCmd : Command {
	u32 firstBinding;
	span<BoundVertexBuffer> buffers;

	std::string toString() const override {
		if(buffers.size() == 1) {
			return dlg::format("BindVertexBuffers({}: {})", firstBinding, name(*buffers[0].buffer));
		} else {
			return dlg::format("BindVertexBuffers({}..{})", firstBinding,
				firstBinding + buffers.size() - 1);
		}
	}

	void displayInspector(Gui&) const override {
		for(auto i = 0u; i < buffers.size(); ++i) {
			auto& buf = *buffers[i].buffer;
			ImGui::Button(dlg::format("{}: {}", firstBinding + i, name(buf)).c_str());
		}
	}

	std::string nameDesc() const override { return "BindVertexBuffers"; }
	Type type() const override { return Type::bind; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct BindIndexBufferCmd : Command {
	Buffer* buffer {};
	VkDeviceSize offset {};
	VkIndexType indexType {};

	std::string nameDesc() const override { return "BindIndexBuffer"; }
	Type type() const override { return Type::bind; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct BindDescriptorSetCmd : Command {
	u32 firstSet;
	VkPipelineBindPoint pipeBindPoint;
	PipelineLayout* pipeLayout; // kept alive via shared_ptr in CommandBuffer
	span<DescriptorSet*> sets;
	span<u32> dynamicOffsets;

	std::string toString() const override {
		if(sets.size() == 1) {
			return dlg::format("BindDescriptorSets({}: {})", firstSet, name(*sets[0]));
		} else {
			return dlg::format("BindDescriptorSets({}..{})",
				firstSet, firstSet + sets.size() - 1);
		}
	}

	std::string nameDesc() const override { return "BindDescriptorSets"; }
	Type type() const override { return Type::bind; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct DispatchCmdBase : Command {
	ComputeState state;
	PushConstantData pushConstants;

	DispatchCmdBase() = default;
	DispatchCmdBase(CommandBuffer&, const ComputeState&);

	Type type() const override { return Type::dispatch; }
	void displayComputeState(Gui& gui) const;
};

struct DispatchCmd : DispatchCmdBase {
	u32 groupsX {};
	u32 groupsY {};
	u32 groupsZ {};

	using DispatchCmdBase::DispatchCmdBase;

	std::string toString() const override {
		return dlg::format("Dispatch({}, {}, {})",
			groupsX, groupsY, groupsZ);
	}

	void displayInspector(Gui& gui) const override {
		imGuiText("Groups: {} {} {}", groupsX, groupsY, groupsZ);
		DispatchCmdBase::displayComputeState(gui);
	}

	std::string nameDesc() const override { return "Dispatch"; }
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
};

struct DispatchIndirectCmd : DispatchCmdBase {
	Buffer* buffer {};
	VkDeviceSize offset {};

	using DispatchCmdBase::DispatchCmdBase;

	std::string toString() const override {
		return "DispatchIndirect";
	}

	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "DispatchIndirect"; }
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
};

struct DispatchBaseCmd : DispatchCmdBase {
	u32 baseGroupX {};
	u32 baseGroupY {};
	u32 baseGroupZ {};
	u32 groupsX {};
	u32 groupsY {};
	u32 groupsZ {};

	using DispatchCmdBase::DispatchCmdBase;

	std::string toString() const override {
		return dlg::format("DispatchBase({}, {}, {}, {}, {}, {})",
			baseGroupX, baseGroupY, baseGroupZ, groupsX, groupsY, groupsZ);
	}

	void displayInspector(Gui& gui) const override {
		imGuiText("Base: {} {} {}", baseGroupX, baseGroupY, baseGroupZ);
		imGuiText("Groups: {} {} {}", groupsX, groupsY, groupsZ);
		DispatchCmdBase::displayComputeState(gui);
	}

	std::string nameDesc() const override { return "DispatchBase"; }
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
};

struct CopyImageCmd : Command {
	Image* src {};
	Image* dst {};
	VkImageLayout srcLayout {};
	VkImageLayout dstLayout {};
	span<VkImageCopy> copies;

	std::string toString() const override {
		return dlg::format("CopyImage({} -> {})", name(*src), name(*dst));
	}

	Type type() const override { return Type::transfer; }

	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "CopyImage"; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct CopyBufferToImageCmd : Command {
	Buffer* src {};
	Image* dst {};
	VkImageLayout imgLayout {};
	span<VkBufferImageCopy> copies;

	std::string toString() const override {
		return dlg::format("CopyBufferToImage({} -> {})", name(*src), name(*dst));
	}

	Type type() const override { return Type::transfer; }
	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "CopyBufferToImage"; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct CopyImageToBufferCmd : Command {
	Image* src {};
	Buffer* dst {};
	VkImageLayout imgLayout {};
	span<VkBufferImageCopy> copies;

	std::string toString() const override {
		return dlg::format("CopyImageToBuffer({} -> {})", name(*src), name(*dst));
	}

	Type type() const override { return Type::transfer; }

	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "CopyImageToBuffer"; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct BlitImageCmd : Command {
	Image* src {};
	Image* dst {};
	VkImageLayout srcLayout {};
	VkImageLayout dstLayout {};
	span<VkImageBlit> blits;
	VkFilter filter {};

	std::string toString() const override {
		return dlg::format("BlitImage({} -> {})", name(*src), name(*dst));
	}

	Type type() const override { return Type::transfer; }

	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "BlitImage"; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct ClearColorImageCmd : Command {
	Image* dst {};
	VkClearColorValue color;
	VkImageLayout imgLayout {};
	span<VkImageSubresourceRange> ranges;

	std::string toString() const override {
		return dlg::format("ClearColorImage({})", name(*dst));
	}

	Type type() const override { return Type::transfer; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct ClearDepthStencilImageCmd : Command {
	Image* dst {};
	VkClearDepthStencilValue value {};
	VkImageLayout imgLayout {};
	span<VkImageSubresourceRange> ranges;

	std::string toString() const override {
		return dlg::format("ClearDepthStencilImage({})", name(*dst));
	}

	Type type() const override { return Type::transfer; }
	std::string nameDesc() const override { return "ClearDepthStencilImage"; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct ClearAttachmentCmd : Command {
	span<VkClearAttachment> attachments;
	span<VkClearRect> rects;

	std::string nameDesc() const override { return "ClearAttachment"; }
	Type type() const override { return Type::transfer; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct ResolveImageCmd : Command {
	Image* src {};
	VkImageLayout srcLayout {};
	Image* dst {};
	VkImageLayout dstLayout {};
	span<VkImageResolve> regions;

	std::string toString() const override { return "ResolveImage"; }
	std::string nameDesc() const override { return "ResolveImage"; }
	Type type() const override { return Type::transfer; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct SetEventCmd : Command {
	Event* event {};
	VkPipelineStageFlags stageMask {};

	std::string toString() const override { return "SetEvent"; }
	std::string nameDesc() const override { return "ResolveImage"; }
	Type type() const override { return Type::sync; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct ResetEventCmd : Command {
	Event* event {};
	VkPipelineStageFlags stageMask {};

	std::string toString() const override { return "ResetEvent"; }
	std::string nameDesc() const override { return "ResolveImage"; }
	Type type() const override { return Type::sync; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct CopyBufferCmd : Command {
	Buffer* src {};
	Buffer* dst {};
	span<VkBufferCopy> regions;

	std::string toString() const override {
		return dlg::format("CopyBuffer({} -> {})", name(*src), name(*dst));
	}

	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "CopyBuffer"; }
	Type type() const override { return Type::transfer; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct UpdateBufferCmd : Command {
	Buffer* dst {};
	VkDeviceSize offset {};
	span<std::byte> data;

	std::string toString() const override {
		return dlg::format("UpdateBuffer({})", name(*dst));
	}

	std::string nameDesc() const override { return "UpdateBuffer"; }
	Type type() const override { return Type::transfer; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct FillBufferCmd : Command {
	Buffer* dst {};
	VkDeviceSize offset {};
	VkDeviceSize size {};
	u32 data {};

	std::string toString() const override {
		return dlg::format("FillBuffer({})", name(*dst));
	}

	std::string nameDesc() const override { return "FillBuffer"; }
	Type type() const override { return Type::transfer; }
	void record(const Device&, VkCommandBuffer) const override;
};

struct ExecuteCommandsCmd : Command {
	span<CommandBuffer*> secondaries {};

	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "ExecuteCommands"; }
	const Command* display(const Command*, TypeFlags) const override;
	void record(const Device&, VkCommandBuffer) const override;
	std::vector<std::string> argumentsDesc() const override;
};

struct BeginDebugUtilsLabelCmd : SectionCommand {
	const char* name {};
	std::array<float, 4> color; // TODO: could use this in UI

	using SectionCommand::SectionCommand;

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

	std::string toString() const override {
		auto bp = (bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) ? "compute" : "graphics";
		return dlg::format("BindPipeline({}, {})", bp, name(*pipe));
	}

	void displayInspector(Gui& gui) const override;
	std::string nameDesc() const override { return "BindPipeline"; }
	Type type() const override { return Type::bind; }
	void record(const Device&, VkCommandBuffer) const override;
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
};

struct EndQueryCmd : Command {
	QueryPool* pool {};
	u32 query {};

	Type type() const override { return Type::query; }
	std::string toString() const override { return "EndQuery"; }
	std::string nameDesc() const override { return "EndQuery"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct ResetQueryPoolCmd : Command {
	QueryPool* pool {};
	u32 first {};
	u32 count {};

	Type type() const override { return Type::query; }
	std::string toString() const override { return "ResetQueryPool"; }
	std::string nameDesc() const override { return "ResetQueryPool"; }
	void record(const Device&, VkCommandBuffer cb) const override;
};

struct WriteTimestampCmd : Command {
	QueryPool* pool {};
	VkPipelineStageFlagBits stage {};
	u32 query {};

	Type type() const override { return Type::query; }
	std::string toString() const override { return "WriteTimestamp"; }
	std::string nameDesc() const override { return "WriteTimestamp"; }
	void record(const Device&, VkCommandBuffer cb) const override;
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
};

} // namespace fuen

#ifdef __GNUC__
	#pragma GCC diagnostic pop
#endif // __GNUC__
