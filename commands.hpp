#pragma once

#include "cb.hpp"
#include "imgui/imgui.h"
#include <vkpp/names.hpp>
#include <dlg/dlg.hpp>

namespace fuen {

// The list of commands in a CommandBuffer is organized as a tree.
// Section-like commands (e.g. cmdBeginRenderPass) have all their commands
// stored as children
struct Command {
	virtual ~Command() = default;

	// Display the command via ImGui
	virtual void display() = 0;
};

// Expects T to be a container over Command pointers
template<typename T>
void displayCommands(const T& container) {
	for(auto& cmd : container) {
		cmd->display();
		ImGui::Separator();
	}
}

struct SectionCommand : Command {
	std::vector<std::unique_ptr<Command>> children;
};

struct BarrierCmdBase : Command {
    VkPipelineStageFlags srcStageMask;
    VkPipelineStageFlags dstStageMask;
	std::vector<VkMemoryBarrier> memBarriers;
	std::vector<VkBufferMemoryBarrier> bufBarriers;
	std::vector<VkImageMemoryBarrier> imgBarriers;
};

struct WaitEventsCmd : BarrierCmdBase {
	// TODO
	// std::vector<Event*> events;
	std::vector<VkEvent> events;

	void display() override {
		ImGui::Text("CmdWaitEvents");
	}
};

struct BarrierCmd : BarrierCmdBase {
    VkDependencyFlags dependencyFlags;

	void display() override {
		ImGui::Text("CmdPipelineBarrier");
	}
};

struct BeginRenderPassCmd : SectionCommand {
	VkRenderPassBeginInfo info;
	Framebuffer* fb;
	RenderPass* rp;

	void display() override {
		if(ImGui::TreeNode(this, "CmdBeginRenderPass")) {
			displayCommands(children);
			ImGui::TreePop();
		}
	}
};

struct NextSubpassCmd : SectionCommand {
	void display() override {
		if(ImGui::TreeNode(this, "CmdNextSubpass")) {
			displayCommands(children);
			ImGui::TreePop();
		}
	}
};

struct EndRenderPassCmd : Command {
	void display() override {
		ImGui::Text("EndRenderPassCmd");
	}
};

struct BaseDrawCmd : Command {
	GraphicsState state;
	PushConstantMap pushConstants;
};

struct DrawCmd : BaseDrawCmd {
	u32 vertexCount;
	u32 instanceCount;
	u32 firstVertex;
	u32 firstInstance;

	void display() override {
		ImGui::Text("CmdDraw(%d, %d, %d, %d)",
			vertexCount, instanceCount, firstVertex, firstInstance);
	}
};

struct DrawIndirectCmd : BaseDrawCmd {
	Buffer* buffer {};
	void display() override {
		ImGui::Text("CmdDrawIndrect");
	}
};

struct DrawIndexedCmd : BaseDrawCmd {
	u32 indexCount;
	u32 instanceCount;
	u32 firstIndex;
	i32 vertexOffset;
	u32 firstInstance;

	void display() override {
		ImGui::Text("CmdDraw(%d, %d, %d, %d, %d)",
			indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
	}
};

struct DrawIndexedIndirectCmd : BaseDrawCmd {
	Buffer* buffer {};
	void display() override {
		ImGui::Text("CmdDrawIndexedIndirect");
	}
};

struct BindVertexBuffersCmd : Command {
	u32 firstBinding;
	std::vector<Buffer*> buffers;

	void display() override {
		ImGui::Text("CmdBindVertexBuffers");
	}
};

struct BindIndexBufferCmd : Command {
	Buffer* buffer {};
	void display() override {
		ImGui::Text("CmdBindIndexBuffer");
	}
};

struct BindDescriptorSetCmd : Command {
	u32 firstSet;
	VkPipelineBindPoint pipeBindPoint;
	PipelineLayout* pipeLayout;
	std::vector<DescriptorSet*> sets;

	void display() override {
		ImGui::Text("CmdBindDescriptorSets");
	}
};

struct BaseDispatchCmd : Command {
	ComputeState state;
	PushConstantMap pushConstants;
};

struct DispatchCmd : BaseDispatchCmd {
	u32 groupsX;
	u32 groupsY;
	u32 groupsZ;

	void display() override {
		ImGui::Text("CmdDispatch(%d, %d, %d)",
			groupsX, groupsY, groupsZ);
	}
};

struct DispatchIndirectCmd : BaseDispatchCmd {
	Buffer* buffer {};

	void display() override {
		ImGui::Text("CmdDispatchIndirect");
	}
};

struct CopyImageCmd : Command {
	Image* src {};
	Image* dst {};
	std::vector<VkImageCopy> copies;

	void display() override {
		ImGui::Text("CmdCopyImage");
	}
};

struct CopyBufferToImageCmd : Command {
	Buffer* src {};
	Image* dst {};
	std::vector<VkBufferImageCopy> copies;

	void display() override {
		ImGui::Text("CmdCopyBufferToImage");
	}
};

struct CopyImageToBufferCmd : Command {
	Image* src {};
	Buffer* dst {};
	std::vector<VkBufferImageCopy> copies;

	void display() override {
		ImGui::Text("CmdCopyImageToBuffer");
	}
};

struct BlitImageCmd : Command {
	Image* src {};
	Image* dst {};
	std::vector<VkImageBlit> blits;
	VkFilter filter;

	void display() override {
		ImGui::Text("CmdBlitImage");
	}
};

struct ClearColorImageCmd : Command {
	Image* dst {};

	void display() override {
		ImGui::Text("CmdClearColorImage");
	}
};

struct CopyBufferCmd : Command {
	Buffer* src {};
	Buffer* dst {};
	std::vector<VkBufferCopy> regions;

	void display() override {
		ImGui::Text("CmdCopyBuffer");
	}
};

struct UpdateBufferCmd : Command {
	Buffer* dst {};
	VkDeviceSize offset {};
	std::vector<std::byte> data;

	void display() override {
		ImGui::Text("CmdUpdateBuffer");
	}
};

struct FillBufferCmd : Command {
	Buffer* dst {};
	VkDeviceSize offset {};
	VkDeviceSize size {};
	u32 data {};

	void display() override {
		ImGui::Text("CmdFillBuffer");
	}
};

struct ExecuteCommandsCmd : Command {
	std::vector<CommandBuffer*> secondaries {};

	void display() override {
		if(ImGui::TreeNode(this, "CmdExecuteCommands")) {
			for(auto* cmd : secondaries) {
				auto label = cmd->name.empty() ? "-" : cmd->name.c_str();
				auto treeID = dlg::format("{}:{}", this, cmd);
				if(ImGui::TreeNode(treeID.c_str(), "%s", label)) {
					displayCommands(cmd->commands);
					ImGui::TreePop();
				}
			}
			ImGui::TreePop();
		}
	}
};

struct BeginDebugUtilsLabelCmd : SectionCommand {
	std::string name;
	std::array<float, 4> color;

	void display() override {
		// auto col = ImVec4 {color[0], color[1], color[2], color[4]};
		// ImGui::PushStyleColor(ImGuiCol_Text, col);
		bool open = ImGui::TreeNode(this, "CmdBeginDebugUtilsLabelEXT: %s", name.c_str());
		// ImGui::PopStyleColor();

		if(open) {
			displayCommands(children);
			ImGui::TreePop();
		}
	}
};

struct EndDebugUtilsLabelCmd : Command {
	void display() override {
		ImGui::Text("CmdEndDebugUtilsLabelEXT");
	}
};

struct BindPipelineCmd : Command {
	VkPipelineBindPoint bindPoint;
	Pipeline* pipe;

	void display() override {
		ImGui::Text("CmdBindPipeline");
	}
};

struct PushConstantsCmd : Command {
	PipelineLayout* layout;
	VkShaderStageFlags stages;
	u32 offset;
	u32 size;
	std::vector<std::byte> values;

	void display() override {
		ImGui::Text("CmdPushConstants");
	}
};

} // namespace fuen
