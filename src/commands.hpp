#pragma once

#include <cbState.hpp>
#include <handles.hpp>
#include <flags.hpp>
#include <imguiutil.hpp>
#include <gui/gui.hpp>
#include <vulkan/vk_enum_string_helper.h>

#include <imgui/imgui.h>
#include <dlg/dlg.hpp>

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
		// Commands that actually cause the gpu to run pipelines
		dispatchDraw = (1u << 3u),
		// Copies, clears and blits
		transfer = (1u << 4u),
		// Commands that end a section
		end = (1u << 5u),
	};

	using TypeFlags = nytl::Flags<Type>;

	virtual ~Command() = default;

	// Display a one-line overview of the command via ImGui.
	// Commands with children should display themselves as tree nodes.
	// Gets the command that is currently selected and should return
	// itself or one of its children, if selected, otherwise nullptr.
	virtual const Command* display(const Command* sel, TypeFlags typeFlags) const {
		if(!(typeFlags & this->type())) {
			return nullptr;
		}

		ImGui::PushID(this);
		ImGui::Bullet();
		auto selected = (sel == this);
		auto ret = ImGui::Selectable(toString().c_str(), selected) ? this : nullptr;
		ImGui::PopID();
		return ret;
	}

	// The name of the command as string (might include parameter information).
	// Used by the default 'display' implementation.
	virtual std::string toString() const { return "<unknown>"; }

	// Whether this command is empty. Empty commands are usually hidden.
	// Used for commands like CmdEndRenderPass.
	virtual Type type() const { return Type::other; }

	// Draws the command inspector UI.
	virtual void displayInspector(Gui&) const {}
};

NYTL_FLAG_OPS(Command::Type)

// Expects T to be a container over Command pointers
template<typename T>
const Command* displayCommands(const T& container, const Command* selected,
		Command::TypeFlags typeFlags) {
	const Command* ret = nullptr;
	for(auto& cmd : container) {
		if(!(typeFlags & cmd->type())) {
			continue;
		}

		ImGui::Separator();
		if(auto reti = cmd->display(selected, typeFlags); reti) {
			dlg_assert(!ret);
			ret = reti;
		}
	}

	return ret;
}

template<typename T>
void resourceRefButton(Gui& gui, T& resource) {
	if(ImGui::Button(name(resource).c_str())) {
		gui.selectResource(resource);
	}
}

struct SectionCommand : Command {
	std::vector<std::unique_ptr<Command>> children;

	const Command* display(const Command* selected, TypeFlags typeFlags) const override {
		auto flags = ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_OpenOnArrow;
		if(this == selected) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		const Command* ret = nullptr;
		if(ImGui::TreeNodeEx(this, flags, "%s", toString().c_str())) {
			if(ImGui::IsItemClicked()) {
				ret = this;
			}

			auto* retc = displayCommands(children, selected, typeFlags);
			if(retc) {
				dlg_assert(!ret);
				ret = retc;
			}

			ImGui::TreePop();
		}

		return ret;
	}
};

struct BarrierCmdBase : Command {
    VkPipelineStageFlags srcStageMask;
    VkPipelineStageFlags dstStageMask;
	std::vector<VkMemoryBarrier> memBarriers;
	std::vector<VkBufferMemoryBarrier> bufBarriers;
	std::vector<VkImageMemoryBarrier> imgBarriers;

	// std::vector<Image*> images;
	// std::vector<Buffer*> buffers;
};

struct WaitEventsCmd : BarrierCmdBase {
	// TODO
	// std::vector<Event*> events;
	std::vector<VkEvent> events;

	std::string toString() const override { return "WaitEvents"; }
	Type type() const override { return Type::sync; }
};

struct BarrierCmd : BarrierCmdBase {
    VkDependencyFlags dependencyFlags;

	std::string toString() const override { return "PipelineBarrier"; }
	Type type() const override { return Type::sync; }
};

struct BeginRenderPassCmd : SectionCommand {
	VkRenderPassBeginInfo info;
	Framebuffer* fb;
	RenderPass* rp;

	std::string toString() const override {
		return dlg::format("BeginRenderPass({}, {})", name(*fb), name(*rp));
	}

	void displayInspector(Gui& gui) const override {
		resourceRefButton(gui, *fb);
		resourceRefButton(gui, *rp);
	}
};

struct NextSubpassCmd : SectionCommand {
	std::string toString() const override { return "NextSubpass"; }
};

struct EndRenderPassCmd : Command {
	std::string toString() const override { return "EndRenderPass"; }
	Type type() const override { return Type::end; }
};

struct BaseDrawCmd : Command {
	GraphicsState state;
	PushConstantMap pushConstants;

	Type type() const override { return Type::dispatchDraw; }

	void displayGrahpicsState(Gui& gui, bool indices) const {
		if(indices) {
			dlg_assert(state.indices.buffer);
			imGuiText("Index Buffer: ");
			ImGui::SameLine();
			resourceRefButton(gui, *state.indices.buffer);
			ImGui::SameLine();
			imGuiText("Offset {}, Type {}", state.indices.offset, string_VkIndexType(state.indices.type));
		}

		resourceRefButton(gui, *state.pipe);

		imGuiText("Verex buffers");
		for(auto& vertBuf : state.vertices) {
			if(!vertBuf.buffer) {
				imGuiText("null");
				continue;
			}

			resourceRefButton(gui, *vertBuf.buffer);
			ImGui::SameLine();
			imGuiText("Offset {}", vertBuf.offset);
		}

		imGuiText("Descriptors");
		for(auto& ds : state.descriptorSets) {
			if(!ds.ds) {
				imGuiText("null");
				continue;
			}

			resourceRefButton(gui, *ds.ds);
			// TODO: dynamic offsets
		}

		// TODO: push constants
	}
};

struct DrawCmd : BaseDrawCmd {
	u32 vertexCount;
	u32 instanceCount;
	u32 firstVertex;
	u32 firstInstance;

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

		BaseDrawCmd::displayGrahpicsState(gui, false);
	}
};

struct DrawIndirectCmd : BaseDrawCmd {
	Buffer* buffer {};

	std::string toString() const override {
		return "DrawIndirect";
	}

	void displayInspector(Gui& gui) const override {
		// TODO: display effective draw command
		resourceRefButton(gui, *buffer);
		BaseDrawCmd::displayGrahpicsState(gui, false);
	}
};

struct DrawIndexedCmd : BaseDrawCmd {
	u32 indexCount;
	u32 instanceCount;
	u32 firstIndex;
	i32 vertexOffset;
	u32 firstInstance;

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

		BaseDrawCmd::displayGrahpicsState(gui, true);
	}
};

struct DrawIndexedIndirectCmd : BaseDrawCmd {
	Buffer* buffer {};

	std::string toString() const override {
		return "DrawIndexedIndirect";
	}

	void displayInspector(Gui& gui) const override {
		// TODO: display effective draw command
		resourceRefButton(gui, *buffer);
		BaseDrawCmd::displayGrahpicsState(gui, true);
	}
};

struct BindVertexBuffersCmd : Command {
	u32 firstBinding;
	std::vector<Buffer*> buffers;

	std::string toString() const override {
		if(buffers.size() == 1) {
			return dlg::format("BindVertexBuffers({}: {})", firstBinding, name(*buffers[0]));
		} else {
			return dlg::format("BindVertexBuffers({}..{})", firstBinding,
				firstBinding + buffers.size() - 1);
		}
	}

	Type type() const override {
		return Type::bind;
	}

	void displayInspector(Gui&) const override {
		for(auto i = 0u; i < buffers.size(); ++i) {
			auto& buf = *buffers[i];
			ImGui::Button(dlg::format("{}: {}", firstBinding + i, name(buf)).c_str());
		}
	}
};

struct BindIndexBufferCmd : Command {
	Buffer* buffer {};

	std::string toString() const override {
		return "BindIndexBuffer";
	}

	Type type() const override {
		return Type::bind;
	}
};

struct BindDescriptorSetCmd : Command {
	u32 firstSet;
	VkPipelineBindPoint pipeBindPoint;
	// PipelineLayout* pipeLayout;
	std::vector<DescriptorSet*> sets;

	std::string toString() const override {
		if(sets.size() == 1) {
			return dlg::format("BindDescriptorSets({}: {})", firstSet, name(*sets[0]));
		} else {
			return dlg::format("BindDescriptorSets({}..{})",
				firstSet, firstSet + sets.size() - 1);
		}
	}

	Type type() const override {
		return Type::bind;
	}
};

struct BaseDispatchCmd : Command {
	ComputeState state;
	PushConstantMap pushConstants;

	Type type() const override { return Type::dispatchDraw; }

	void displayComputeState(Gui& gui) const {
		resourceRefButton(gui, *state.pipe);

		imGuiText("Descriptors");
		for(auto& ds : state.descriptorSets) {
			if(!ds.ds) {
				imGuiText("null");
				continue;
			}

			resourceRefButton(gui, *ds.ds);
			// TODO: dynamic offsets
		}

		// TODO: push constants
	}
};

struct DispatchCmd : BaseDispatchCmd {
	u32 groupsX;
	u32 groupsY;
	u32 groupsZ;

	std::string toString() const override {
		return dlg::format("Dispatch({}, {}, {})",
			groupsX, groupsY, groupsZ);
	}

	void displayInspector(Gui& gui) const override {
		imGuiText("Groups: {} {} {}", groupsX, groupsY, groupsZ);
		BaseDispatchCmd::displayComputeState(gui);
	}
};

struct DispatchIndirectCmd : BaseDispatchCmd {
	Buffer* buffer {};

	std::string toString() const override {
		return "DispatchIndirect";
	}

	void displayInspector(Gui& gui) const override {
		// TODO: display effective dispatch command
		resourceRefButton(gui, *buffer);
		BaseDispatchCmd::displayComputeState(gui);
	}
};

struct CopyImageCmd : Command {
	Image* src {};
	Image* dst {};
	std::vector<VkImageCopy> copies;

	std::string toString() const override {
		return dlg::format("CopyImage({} -> {})", name(*src), name(*dst));
	}

	Type type() const override { return Type::transfer; }

	void displayInspector(Gui& gui) const override {
		resourceRefButton(gui, *src);
		ImGui::SameLine();
		imGuiText(" -> ");
		ImGui::SameLine();
		resourceRefButton(gui, *dst);

		// TODO: copies
	}
};

struct CopyBufferToImageCmd : Command {
	Buffer* src {};
	Image* dst {};
	std::vector<VkBufferImageCopy> copies;

	std::string toString() const override {
		return dlg::format("CopyBufferToImage({} -> {})", name(*src), name(*dst));
	}

	Type type() const override { return Type::transfer; }

	void displayInspector(Gui& gui) const override {
		resourceRefButton(gui, *src);
		ImGui::SameLine();
		imGuiText(" -> ");
		ImGui::SameLine();
		resourceRefButton(gui, *dst);

		// TODO: copies
	}
};

struct CopyImageToBufferCmd : Command {
	Image* src {};
	Buffer* dst {};
	std::vector<VkBufferImageCopy> copies;

	std::string toString() const override {
		return dlg::format("CopyImageToBuffer({} -> {})", name(*src), name(*dst));
	}

	Type type() const override { return Type::transfer; }

	void displayInspector(Gui& gui) const override {
		resourceRefButton(gui, *src);
		ImGui::SameLine();
		imGuiText(" -> ");
		ImGui::SameLine();
		resourceRefButton(gui, *dst);

		// TODO: copies
	}
};

struct BlitImageCmd : Command {
	Image* src {};
	Image* dst {};
	std::vector<VkImageBlit> blits;
	VkFilter filter;

	std::string toString() const override {
		return dlg::format("BlitImage({} -> {})", name(*src), name(*dst));
	}

	Type type() const override { return Type::transfer; }

	void displayInspector(Gui& gui) const override {
		resourceRefButton(gui, *src);
		ImGui::SameLine();
		imGuiText(" -> ");
		ImGui::SameLine();
		resourceRefButton(gui, *dst);

		// TODO: filter
		// TODO: blits
	}
};

struct ClearColorImageCmd : Command {
	Image* dst {};
	VkClearColorValue color;
	std::vector<VkImageSubresourceRange> ranges;

	std::string toString() const override {
		return dlg::format("ClearColorImage({})", name(*dst));
	}

	Type type() const override { return Type::transfer; }
};

struct CopyBufferCmd : Command {
	Buffer* src {};
	Buffer* dst {};
	std::vector<VkBufferCopy> regions;

	std::string toString() const override {
		return dlg::format("CopyBuffer({} -> {})", name(*src), name(*dst));
	}

	Type type() const override { return Type::transfer; }

	void displayInspector(Gui& gui) const override {
		resourceRefButton(gui, *src);
		ImGui::SameLine();
		imGuiText(" -> ");
		ImGui::SameLine();
		resourceRefButton(gui, *dst);

		// TODO: copies
	}
};

struct UpdateBufferCmd : Command {
	Buffer* dst {};
	VkDeviceSize offset {};
	std::vector<std::byte> data;

	std::string toString() const override {
		return dlg::format("UpdateBuffer({})", name(*dst));
	}

	Type type() const override { return Type::transfer; }
};

struct FillBufferCmd : Command {
	Buffer* dst {};
	VkDeviceSize offset {};
	VkDeviceSize size {};
	u32 data {};

	std::string toString() const override {
		return dlg::format("FillBuffer({})", name(*dst));
	}

	Type type() const override { return Type::transfer; }
};

struct ExecuteCommandsCmd : Command {
	std::vector<CommandBuffer*> secondaries {};

	const Command* display(const Command*, TypeFlags) const override;

	void displayInspector(Gui& gui) const override {
		for(auto* cb : secondaries) {
			resourceRefButton(gui, *cb);
		}
	}
};

struct BeginDebugUtilsLabelCmd : SectionCommand {
	std::string name;
	std::array<float, 4> color; // TODO: could use this in UI

	std::string toString() const override {
		return dlg::format("Label: {}", name);
	}
};

struct EndDebugUtilsLabelCmd : Command {
	std::string toString() const override {
		return dlg::format("EndLabel");
	}

	Type type() const override { return Type::end; }
};

struct BindPipelineCmd : Command {
	VkPipelineBindPoint bindPoint;
	Pipeline* pipe;

	std::string toString() const override {
		auto bp = (bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) ? "compute" : "graphics";
		return dlg::format("BindPipeline({}, {})", bp, name(*pipe));
	}

	Type type() const override { return Type::bind; }

	void displayInspector(Gui& gui) const override {
		dlg_assert(pipe->type == bindPoint);
		if(bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
			resourceRefButton(gui, *static_cast<ComputePipeline*>(pipe));
		} else if(bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
			resourceRefButton(gui, *static_cast<GraphicsPipeline*>(pipe));
		}
	}
};

struct PushConstantsCmd : Command {
	// PipelineLayout* layout;
	VkShaderStageFlags stages;
	u32 offset;
	u32 size;
	std::vector<std::byte> values;

	std::string toString() const override {
		return "PushConstants";
	}

	Type type() const override { return Type::bind; }
};

} // namespace fuen
