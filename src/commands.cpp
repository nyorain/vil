#include <commands.hpp>
#include <cb.hpp>
#include <span.hpp>

namespace fuen {

const Command* ExecuteCommandsCmd::display(const Command* selected, TypeFlags typeFlags) const {
	auto flags = ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_OpenOnArrow;
	if(this == selected) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}

	const Command* ret = nullptr;
	if(ImGui::TreeNodeEx(this, flags, "CmdExecuteCommands")) {
		if(ImGui::IsItemClicked()) {
			ret = this;
		}

		// The most common use case: one single command buffer is executed.
		// In that case we show a simpler UI
		if(secondaries.size() == 1) {
			auto* cmd = secondaries[0];
			auto reti = displayCommands(cmd->commands, selected, typeFlags);
			if(reti) {
				dlg_assert(!ret);
				ret = reti;
			}
		} else {
			for(auto* cmd : secondaries) {
				auto cname = name(*cmd);
				auto treeID = dlg::format("{}:{}", this, cname);
				if(ImGui::TreeNode(treeID.c_str(), "%s", cname.c_str())) {
					auto reti = displayCommands(cmd->commands, selected, typeFlags);
					if(reti) {
						dlg_assert(!ret);
						ret = reti;
					}

					ImGui::TreePop();
				}
			}
		}

		ImGui::TreePop();
	}

	return ret;
}

// recording
void WaitEventsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdWaitEvents(cb,
		this->events.size(), this->events.data(),
		this->srcStageMask, this->dstStageMask,
		this->memBarriers.size(), this->memBarriers.data(),
		this->bufBarriers.size(), this->bufBarriers.data(),
		this->imgBarriers.size(), this->imgBarriers.data());

}

void BarrierCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdPipelineBarrier(cb,
		this->dependencyFlags,
		this->srcStageMask, this->dstStageMask,
		this->memBarriers.size(), this->memBarriers.data(),
		this->bufBarriers.size(), this->bufBarriers.data(),
		this->imgBarriers.size(), this->imgBarriers.data());

}

void BeginRenderPassCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdBeginRenderPass(cb, &this->info, this->subpassContents);
}

void NextSubpassCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdNextSubpass(cb, this->subpassContents);
}

void EndRenderPassCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdEndRenderPass(cb);
}

void DrawCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDraw(cb, vertexCount, instanceCount, firstVertex, firstInstance);
}

void DrawIndirectCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDrawIndirect(cb, buffer->handle, offset, drawCount, stride);
}

void DrawIndexedCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDrawIndexed(cb, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void DrawIndexedIndirectCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDrawIndexedIndirect(cb, buffer->handle, offset, drawCount, stride);
}

template<typename H>
auto rawHandles(const std::vector<H*>& handles) {
	using VkH = decltype(handles[0]->handle);
	std::vector<VkH> ret;
	ret.reserve(handles.size());
	for(auto* h : handles) {
		ret.push_back(h->handle);
	}

	return ret;
}

void BindVertexBuffersCmd::record(const Device& dev, VkCommandBuffer cb) const {
	auto vkbuffers = rawHandles(buffers);
	dev.dispatch.CmdBindVertexBuffers(cb, firstBinding,
		vkbuffers.size(), vkbuffers.data(), offsets.data());
}

void BindIndexBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdBindIndexBuffer(cb, buffer->handle, offset, indexType);
}

void BindDescriptorSetCmd::record(const Device& dev, VkCommandBuffer cb) const {
	auto vkds = rawHandles(sets);
	dev.dispatch.CmdBindDescriptorSets(cb, pipeBindPoint, pipeLayout->handle,
		firstSet, vkds.size(), vkds.data(),
		dynamicOffsets.size(), dynamicOffsets.data());
}

void DispatchCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDispatch(cb, groupsX, groupsY, groupsZ);
}

void DispatchIndirectCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDispatchIndirect(cb, buffer->handle, offset);
}

void CopyImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyImage(cb, src->handle, srcLayout, dst->handle, dstLayout,
		copies.size(), copies.data());
}

void CopyBufferToImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyBufferToImage(cb, src->handle, dst->handle,
		imgLayout, copies.size(), copies.data());
}

void CopyImageToBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyImageToBuffer(cb, src->handle, imgLayout, dst->handle,
		copies.size(), copies.data());
}

void BlitImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdBlitImage(cb, src->handle, srcLayout, dst->handle, dstLayout,
		blits.size(), blits.data(), filter);
}

void ClearColorImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdClearColorImage(cb, dst->handle, imgLayout, &color,
		ranges.size(), ranges.data());
}

void CopyBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyBuffer(cb, src->handle, dst->handle,
		regions.size(), regions.data());
}

void UpdateBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdUpdateBuffer(cb, dst->handle, offset, data.size(), data.data());
}

void FillBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdFillBuffer(cb, dst->handle, offset, size, data);
}

void ExecuteCommandsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	auto vkcbs = rawHandles(secondaries);
	dev.dispatch.CmdExecuteCommands(cb, vkcbs.size(), vkcbs.data());
}

void BeginDebugUtilsLabelCmd::record(const Device& dev, VkCommandBuffer cb) const {
	VkDebugUtilsLabelEXT label {};
	label.pLabelName = name.c_str();
	std::memcpy(&label.color, this->color.data(), sizeof(label.color));
	dev.dispatch.CmdBeginDebugUtilsLabelEXT(cb, &label);
}

void EndDebugUtilsLabelCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdEndDebugUtilsLabelEXT(cb);
}

void BindPipelineCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdBindPipeline(cb, bindPoint, pipe->handle);
}

void PushConstantsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdPushConstants(cb, layout->handle, stages, offset,
		values.size(), values.data());
}

void SetViewportCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetViewport(cb, first, viewports.size(), viewports.data());
}

void SetScissorCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetScissor(cb, first, scissors.size(), scissors.data());
}

void SetLineWidthCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetLineWidth(cb, width);
}

void SetDepthBiasCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetDepthBias(cb, state.constant, state.clamp, state.slope);
}

void SetBlendConstantsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetBlendConstants(cb, values.data());
}

void SetStencilCompareMaskCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetStencilCompareMask(cb, faceMask, value);
}

void SetStencilWriteMaskCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetStencilWriteMask(cb, faceMask, value);
}

void SetStencilReferenceCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetStencilReference(cb, faceMask, value);
}

void SetDepthBoundsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetDepthBounds(cb, min, max);
}

} // namespace fuen
