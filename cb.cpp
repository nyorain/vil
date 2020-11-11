#include "cb.hpp"
#include "data.hpp"
#include "rp.hpp"
#include "ds.hpp"
#include "buffer.hpp"
#include "image.hpp"
#include "imgui/imgui.h"
#include <vkpp/names.hpp>

namespace fuen {

// commands
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

struct BeginRenderPassCmd : Command {
	VkRenderPassBeginInfo info;
	Framebuffer* fb;
	RenderPass* rp;

	void display() override {
		ImGui::Text("CmdBeginRenderPass");
	}
};

struct DrawCmd : Command {
	u32 vertexCount;
	u32 instanceCount;
	u32 firstVertex;
	u32 firstInstance;

	void display() override {
		ImGui::Text("CmdDraw(%d, %d, %d, %d)",
			vertexCount, instanceCount, firstVertex, firstInstance);
	}
};

struct DrawIndirectCmd : Command {
	Buffer* buffer {};
	void display() override {
		ImGui::Text("CmdDrawIndrect");
	}
};

struct DrawIndexedCmd : Command {
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

struct DrawIndexedIndirectCmd : Command {
	Buffer* buffer {};
	void display() override {
		ImGui::Text("CmdDrawIndexedIndirect");
	}
};

struct BindVertexBuffersCmd : Command {
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
	VkPipelineLayout pipeLayout;
	std::vector<DescriptorSet*> sets;

	void display() override {
		ImGui::Text("CmdBindDescriptorSets");
	}
};

struct DispatchCmd : Command {
	u32 groupsX;
	u32 groupsY;
	u32 groupsZ;

	void display() override {
		ImGui::Text("CmdDispatch(%d, %d, %d)",
			groupsX, groupsY, groupsZ);
	}
};

struct DispatchIndirectCmd : Command {
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
		ImGui::Text("CmdExecuteCommands");
	}
};

// util
void reset(CommandBuffer& cb) {
	cb.buffers.clear();
	cb.images.clear();
	cb.commands.clear();
}

// api
// command pool
VKAPI_ATTR VkResult VKAPI_CALL CreateCommandPool(
		VkDevice                                    device,
		const VkCommandPoolCreateInfo*              pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkCommandPool*                              pCommandPool) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.vkCreateCommandPool(device, pCreateInfo, pAllocator, pCommandPool);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& cp = dev.commandPools.add(*pCommandPool);
	cp.dev = &dev;
	cp.pool = *pCommandPool;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(
		VkDevice                                    device,
		VkCommandPool                               commandPool,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	auto& cp = dev.commandPools.get(commandPool);

	for(auto* cb : cp.cbs) {
		eraseData(cb->cb);
		dev.commandBuffers.mustErase(cb->cb);
	}

	dev.commandPools.mustErase(commandPool);
	dev.dispatch.vkDestroyCommandPool(device, commandPool, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL ResetCommandPool(
		VkDevice                                    device,
		VkCommandPool                               commandPool,
		VkCommandPoolResetFlags                     flags) {
	auto& dev = getData<Device>(device);
	auto& cp = dev.commandPools.get(commandPool);
	for(auto* cb : cp.cbs) {
		reset(*cb);
	}
	return dev.dispatch.vkResetCommandPool(device, commandPool, flags);
}

// command buffer
VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(
		VkDevice                                    device,
		const VkCommandBufferAllocateInfo*          pAllocateInfo,
		VkCommandBuffer*                            pCommandBuffers) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.vkAllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto* cp = dev.commandPools.find(pAllocateInfo->commandPool);
	for(auto i = 0u; i < pAllocateInfo->commandBufferCount; ++i) {
		auto& cb = dev.commandBuffers.add(pCommandBuffers[i]);
		cb.dev = &dev;
		cb.cb = pCommandBuffers[i];
		cb.pool = cp;
		cb.pool->cbs.push_back(&cb);

		// command buffers are dispatchable, add global data entry
		fuen::insertData(pCommandBuffers[i], &cb);
	}

	return res;
}

VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers(
		VkDevice                                    device,
		VkCommandPool                               commandPool,
		uint32_t                                    commandBufferCount,
		const VkCommandBuffer*                      pCommandBuffers) {
	auto& dev = getData<Device>(device);
	dev.dispatch.vkFreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);

	for(auto i = 0u; i < commandBufferCount; ++i) {
		auto& cb = dev.commandBuffers.get(pCommandBuffers[i]);
		if(cb.pool) {
			dlg_assert(cb.pool == dev.commandPools.find(commandPool));
			auto it = std::find(cb.pool->cbs.begin(), cb.pool->cbs.end(), &cb);
			dlg_assert(it != cb.pool->cbs.end());
			cb.pool->cbs.erase(it);
		}

		eraseData(pCommandBuffers[i]);
		dev.commandBuffers.mustErase(pCommandBuffers[i]);
	}
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(
		VkCommandBuffer                             commandBuffer,
		const VkCommandBufferBeginInfo*             pBeginInfo) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	reset(cb);
	return cb.dev->dispatch.vkBeginCommandBuffer(commandBuffer, pBeginInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL EndCommandBuffer(
		VkCommandBuffer                             commandBuffer) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	return cb.dev->dispatch.vkEndCommandBuffer(commandBuffer);
}

VKAPI_ATTR VkResult VKAPI_CALL ResetCommandBuffer(
		VkCommandBuffer                             commandBuffer,
		VkCommandBufferResetFlags                   flags) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	reset(cb);
	return cb.dev->dispatch.vkResetCommandBuffer(commandBuffer, flags);
}

// == command buffer recording ==
// util
CommandBuffer::UsedImage& useImage(CommandBuffer& cb, Command& cmd, Image& image) {
	auto& img = cb.images[image.handle];
	img.commands.push_back(&cmd);
	if(!img.image) {
		img.image = &image;
		dlg_assert(img.image);
	}

	return img;
}

CommandBuffer::UsedImage& useImage(CommandBuffer& cb, Command& cmd, VkImage image) {
	auto& img = cb.images[image];
	img.commands.push_back(&cmd);
	if(!img.image) {
		img.image = &cb.dev->images.get(image);
	}

	return img;
}

CommandBuffer::UsedImage& useImage(CommandBuffer& cb, Command& cmd, VkImage image,
		VkImageLayout newLayout) {
	auto& img = useImage(cb, cmd, image);
	img.layoutChanged = true;
	img.finalLayout = newLayout;
	return img;
}

CommandBuffer::UsedImage& useImage(CommandBuffer& cb, Command& cmd, Image& image,
		VkImageLayout newLayout) {
	auto& img = useImage(cb, cmd, image);
	img.layoutChanged = true;
	img.finalLayout = newLayout;
	return img;
}

CommandBuffer::UsedBuffer& useBuffer(CommandBuffer& cb, Command& cmd, Buffer& buf) {
	auto& useBuf = cb.buffers[buf.handle];
	useBuf.buffer = &buf;
	useBuf.commands.push_back(&cmd);
	return useBuf;
}

void cmdBarrier(
		CommandBuffer& cb,
		BarrierCmdBase& cmd,
		VkPipelineStageFlags                        srcStageMask,
		VkPipelineStageFlags                        dstStageMask,
		uint32_t                                    memoryBarrierCount,
		const VkMemoryBarrier*                      pMemoryBarriers,
		uint32_t                                    bufferMemoryBarrierCount,
		const VkBufferMemoryBarrier*                pBufferMemoryBarriers,
		uint32_t                                    imageMemoryBarrierCount,
		const VkImageMemoryBarrier*                 pImageMemoryBarriers) {
	cmd.srcStageMask = srcStageMask;
	cmd.dstStageMask = dstStageMask;
	cmd.memBarriers = {pMemoryBarriers, pMemoryBarriers + memoryBarrierCount};
	cmd.imgBarriers = {pImageMemoryBarriers, pImageMemoryBarriers + imageMemoryBarrierCount};
	cmd.bufBarriers = {pBufferMemoryBarriers, pBufferMemoryBarriers + bufferMemoryBarrierCount};

	for(auto& imgb : cmd.imgBarriers) {
		useImage(cb, cmd, imgb.image, imgb.newLayout);
	}

	for(auto& buf : cmd.bufBarriers) {
		auto& bbuf = cb.dev->buffers.get(buf.buffer);
		useBuffer(cb, cmd, bbuf);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdWaitEvents(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    eventCount,
		const VkEvent*                              pEvents,
		VkPipelineStageFlags                        srcStageMask,
		VkPipelineStageFlags                        dstStageMask,
		uint32_t                                    memoryBarrierCount,
		const VkMemoryBarrier*                      pMemoryBarriers,
		uint32_t                                    bufferMemoryBarrierCount,
		const VkBufferMemoryBarrier*                pBufferMemoryBarriers,
		uint32_t                                    imageMemoryBarrierCount,
		const VkImageMemoryBarrier*                 pImageMemoryBarriers) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<WaitEventsCmd>();
	cmd->events = {pEvents, pEvents + eventCount};
	cmdBarrier(cb, *cmd, srcStageMask, dstStageMask,
		memoryBarrierCount, pMemoryBarriers,
		bufferMemoryBarrierCount, pBufferMemoryBarriers,
		imageMemoryBarrierCount, pImageMemoryBarriers);

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdWaitEvents(commandBuffer, eventCount, pEvents,
		srcStageMask, dstStageMask,
		memoryBarrierCount, pMemoryBarriers,
		bufferMemoryBarrierCount, pBufferMemoryBarriers,
		imageMemoryBarrierCount, pImageMemoryBarriers);
}

VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(
		VkCommandBuffer                             commandBuffer,
		VkPipelineStageFlags                        srcStageMask,
		VkPipelineStageFlags                        dstStageMask,
		VkDependencyFlags                           dependencyFlags,
		uint32_t                                    memoryBarrierCount,
		const VkMemoryBarrier*                      pMemoryBarriers,
		uint32_t                                    bufferMemoryBarrierCount,
		const VkBufferMemoryBarrier*                pBufferMemoryBarriers,
		uint32_t                                    imageMemoryBarrierCount,
		const VkImageMemoryBarrier*                 pImageMemoryBarriers) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<BarrierCmd>();
	cmd->dependencyFlags = dependencyFlags;
	cmdBarrier(cb, *cmd, srcStageMask, dstStageMask,
		memoryBarrierCount, pMemoryBarriers,
		bufferMemoryBarrierCount, pBufferMemoryBarriers,
		imageMemoryBarrierCount, pImageMemoryBarriers);

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdPipelineBarrier(commandBuffer,
		srcStageMask, dstStageMask, dependencyFlags,
		memoryBarrierCount, pMemoryBarriers,
		bufferMemoryBarrierCount, pBufferMemoryBarriers,
		imageMemoryBarrierCount, pImageMemoryBarriers);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(
		VkCommandBuffer                             commandBuffer,
		const VkRenderPassBeginInfo*                pRenderPassBegin,
		VkSubpassContents                           contents) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<BeginRenderPassCmd>();
	cmd->info = *pRenderPassBegin;
	cmd->fb = cb.dev->framebuffers.find(pRenderPassBegin->framebuffer);
	cmd->rp = cb.dev->renderPasses.find(pRenderPassBegin->renderPass);

	dlg_assert(cmd->fb);
	dlg_assert(cmd->rp);
	if(cmd->fb && cmd->rp) {
		dlg_assert(cmd->rp->info.attachments.size() == cmd->fb->attachments.size());
		for(auto i = 0u; i < cmd->fb->attachments.size(); ++i) {
			auto& attachment = cmd->fb->attachments[i];
			if(!attachment || !attachment->img) {
				continue;
			}

			// TODO: can there be barriers inside the renderpasss?
			//   maybe better move this to RenderPassEnd?
			// TODO: handle secondary command buffers and stuff
			useImage(cb, *cmd, *attachment->img,
				cmd->rp->info.attachments[i].finalLayout);
		}
	}

	cb.commands.push_back(std::move(cmd));

	cb.dev->dispatch.vkCmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
}

VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(
		VkCommandBuffer                             commandBuffer,
		VkPipelineBindPoint                         pipelineBindPoint,
		VkPipelineLayout                            layout,
		uint32_t                                    firstSet,
		uint32_t                                    descriptorSetCount,
		const VkDescriptorSet*                      pDescriptorSets,
		uint32_t                                    dynamicOffsetCount,
		const uint32_t*                             pDynamicOffsets) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<BindDescriptorSetCmd>();

	cmd->firstSet = firstSet;
	cmd->pipeBindPoint = pipelineBindPoint;
	cmd->pipeLayout = layout;

	// TODO: we could push this to the point where the descriptor set is
	// acutally used, in case it is just bound without usage.
	for(auto i = 0u; i < descriptorSetCount; ++i) {
		auto& ds = cb.dev->descriptorSets.get(pDescriptorSets[i]);

		for(auto b = 0u; b < ds.bindings.size(); ++b) {
			auto cat = category(ds.layout->bindings[b].descriptorType);
			if(cat == DescriptorCategory::image) {
				for(auto e = 0u; e < ds.bindings[b].size(); ++e) {
					if(!ds.bindings[b][e].valid || !ds.bindings[b][e].imageInfo.imageView) {
						continue;
					}

					auto& view = cb.dev->imageViews.get(ds.bindings[b][e].imageInfo.imageView);
					useImage(cb, *cmd, *view.img);
				}
			} else if(cat == DescriptorCategory::buffer) {
				for(auto e = 0u; e < ds.bindings[b].size(); ++e) {
					if(!ds.bindings[b][e].valid || !ds.bindings[b][e].bufferInfo.buffer) {
						continue;
					}

					auto& buf = cb.dev->buffers.get(ds.bindings[b][e].bufferInfo.buffer);
					useBuffer(cb, *cmd, buf);
				}
			} // TODO: buffer view
		}

		cmd->sets.push_back(&ds);
	}

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdBindDescriptorSets(commandBuffer,
		pipelineBindPoint,
		layout,
		firstSet,
		descriptorSetCount,
		pDescriptorSets,
		dynamicOffsetCount,
		pDynamicOffsets);
}

VKAPI_ATTR void VKAPI_CALL CmdBindIndexBuffer(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    buffer,
		VkDeviceSize                                offset,
		VkIndexType                                 indexType) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<BindIndexBufferCmd>();

	auto& buf = cb.dev->buffers.get(buffer);
	cmd->buffer = &buf;
	useBuffer(cb, *cmd, buf);

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdBindIndexBuffer(commandBuffer,
		buffer, offset, indexType);
}

VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    firstBinding,
		uint32_t                                    bindingCount,
		const VkBuffer*                             pBuffers,
		const VkDeviceSize*                         pOffsets) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<BindVertexBuffersCmd>();

	for(auto i = 0u; i < bindingCount; ++i) {
		auto& buf = cb.dev->buffers.get(pBuffers[i]);
		cmd->buffers.push_back(&buf);
		useBuffer(cb, *cmd, buf);
	}

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdBindVertexBuffers(commandBuffer,
		firstBinding, bindingCount, pBuffers, pOffsets);
}

VKAPI_ATTR void VKAPI_CALL CmdDraw(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    vertexCount,
		uint32_t                                    instanceCount,
		uint32_t                                    firstVertex,
		uint32_t                                    firstInstance) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<DrawCmd>();

	cmd->vertexCount = vertexCount;
	cmd->instanceCount = instanceCount;
	cmd->firstVertex = firstVertex;
	cmd->firstInstance = firstInstance;

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdDraw(commandBuffer,
		vertexCount, instanceCount, firstVertex, firstInstance);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    indexCount,
		uint32_t                                    instanceCount,
		uint32_t                                    firstIndex,
		int32_t                                     vertexOffset,
		uint32_t                                    firstInstance) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<DrawIndexedCmd>();

	cmd->firstInstance = firstInstance;
	cmd->instanceCount = instanceCount;
	cmd->indexCount = indexCount;
	cmd->vertexOffset = vertexOffset;
	cmd->firstIndex = firstIndex;

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdDrawIndexed(commandBuffer,
		indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndirect(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    buffer,
		VkDeviceSize                                offset,
		uint32_t                                    drawCount,
		uint32_t                                    stride) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<DrawIndirectCmd>();

	auto& buf = cb.dev->buffers.get(buffer);
	cmd->buffer = &buf;
	useBuffer(cb, *cmd, buf);

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdDrawIndirect(commandBuffer,
		buffer, offset, drawCount, stride);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirect(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    buffer,
		VkDeviceSize                                offset,
		uint32_t                                    drawCount,
		uint32_t                                    stride) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<DrawIndexedIndirectCmd>();

	auto& buf = cb.dev->buffers.get(buffer);
	cmd->buffer = &buf;
	useBuffer(cb, *cmd, buf);

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdDrawIndexedIndirect(commandBuffer,
		buffer, offset, drawCount, stride);
}

VKAPI_ATTR void VKAPI_CALL CmdDispatch(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    groupCountX,
		uint32_t                                    groupCountY,
		uint32_t                                    groupCountZ) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<DispatchCmd>();

	cmd->groupsX = groupCountX;
	cmd->groupsY = groupCountY;
	cmd->groupsZ = groupCountZ;

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdDispatch(commandBuffer,
		groupCountX, groupCountY, groupCountZ);
}

VKAPI_ATTR void VKAPI_CALL CmdDispatchIndirect(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    buffer,
		VkDeviceSize                                offset) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<DispatchIndirectCmd>();

	auto& buf = cb.dev->buffers.get(buffer);
	cmd->buffer = &buf;
	useBuffer(cb, *cmd, buf);

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdDispatchIndirect(commandBuffer, buffer, offset);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyImage(
		VkCommandBuffer                             commandBuffer,
		VkImage                                     srcImage,
		VkImageLayout                               srcImageLayout,
		VkImage                                     dstImage,
		VkImageLayout                               dstImageLayout,
		uint32_t                                    regionCount,
		const VkImageCopy*                          pRegions) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<CopyImageCmd>();

	auto& src = cb.dev->images.get(srcImage);
	auto& dst = cb.dev->images.get(dstImage);

	cmd->src = &src;
	cmd->dst = &dst;
	cmd->copies = {pRegions, pRegions + regionCount};

	useImage(cb, *cmd, src);
	useImage(cb, *cmd, dst);

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdCopyImage(commandBuffer,
		srcImage, srcImageLayout,
		dstImage, dstImageLayout,
		regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL CmdBlitImage(
		VkCommandBuffer                             commandBuffer,
		VkImage                                     srcImage,
		VkImageLayout                               srcImageLayout,
		VkImage                                     dstImage,
		VkImageLayout                               dstImageLayout,
		uint32_t                                    regionCount,
		const VkImageBlit*                          pRegions,
		VkFilter                                    filter) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<BlitImageCmd>();

	auto& src = cb.dev->images.get(srcImage);
	auto& dst = cb.dev->images.get(dstImage);

	cmd->src = &src;
	cmd->dst = &dst;
	cmd->blits = {pRegions, pRegions + regionCount};
	cmd->filter = filter;

	useImage(cb, *cmd, src);
	useImage(cb, *cmd, dst);

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdBlitImage(commandBuffer,
		srcImage, srcImageLayout,
		dstImage, dstImageLayout,
		regionCount, pRegions, filter);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyBufferToImage(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    srcBuffer,
		VkImage                                     dstImage,
		VkImageLayout                               dstImageLayout,
		uint32_t                                    regionCount,
		const VkBufferImageCopy*                    pRegions) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<CopyBufferToImageCmd>();

	auto& src = cb.dev->buffers.get(srcBuffer);
	auto& dst = cb.dev->images.get(dstImage);

	cmd->src = &src;
	cmd->dst = &dst;
	cmd->copies = {pRegions, pRegions + regionCount};

	useBuffer(cb, *cmd, src);
	useImage(cb, *cmd, dst);

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdCopyBufferToImage(commandBuffer,
		srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyImageToBuffer(
		VkCommandBuffer                             commandBuffer,
		VkImage                                     srcImage,
		VkImageLayout                               srcImageLayout,
		VkBuffer                                    dstBuffer,
		uint32_t                                    regionCount,
		const VkBufferImageCopy*                    pRegions) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<CopyImageToBufferCmd>();

	auto& src = cb.dev->images.get(srcImage);
	auto& dst = cb.dev->buffers.get(dstBuffer);

	cmd->src = &src;
	cmd->dst = &dst;
	cmd->copies = {pRegions, pRegions + regionCount};

	useImage(cb, *cmd, src);
	useBuffer(cb, *cmd, dst);

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdCopyImageToBuffer(commandBuffer,
		srcImage, srcImageLayout, dstBuffer, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL CmdClearColorImage(
		VkCommandBuffer                             commandBuffer,
		VkImage                                     image,
		VkImageLayout                               imageLayout,
		const VkClearColorValue*                    pColor,
		uint32_t                                    rangeCount,
		const VkImageSubresourceRange*              pRanges) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<ClearColorImageCmd>();

	auto& dst = cb.dev->images.get(image);
	cmd->dst = &dst;

	useImage(cb, *cmd, dst);

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdClearColorImage(commandBuffer,
		image, imageLayout, pColor, rangeCount, pRanges);
}

VKAPI_ATTR void VKAPI_CALL CmdExecuteCommands(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    commandBufferCount,
		const VkCommandBuffer*                      pCommandBuffers) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<ExecuteCommandsCmd>();

	for(auto i = 0u; i < commandBufferCount; ++i) {
		auto& secondary = cb.dev->commandBuffers.get(pCommandBuffers[i]);
		cmd->secondaries.push_back(&secondary);

		for(auto& img : secondary.images) {
			auto& useImg = cb.images[img.second.image->handle];
			useImg.image = img.second.image;
			useImg.commands.push_back(cmd.get());

			if(img.second.layoutChanged) {
				useImg.layoutChanged = true;
				useImg.finalLayout = img.second.finalLayout;
			}
		}

		for(auto& buf : secondary.buffers) {
			auto& useBuf = cb.buffers[buf.second.buffer->handle];
			useBuf.buffer = buf.second.buffer;
			useBuf.commands.push_back(cmd.get());
		}
	}

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdExecuteCommands(commandBuffer,
		commandBufferCount, pCommandBuffers);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    srcBuffer,
		VkBuffer                                    dstBuffer,
		uint32_t                                    regionCount,
		const VkBufferCopy*                         pRegions) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<CopyBufferCmd>();

	auto& srcBuf = cb.dev->buffers.get(srcBuffer);
	auto& dstBuf = cb.dev->buffers.get(dstBuffer);

	cmd->src = &srcBuf;
	cmd->dst = &dstBuf;
	cmd->regions = {pRegions, pRegions + regionCount};

	useBuffer(cb, *cmd, srcBuf);
	useBuffer(cb, *cmd, dstBuf);

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdCopyBuffer(commandBuffer,
		srcBuffer, dstBuffer, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL CmdUpdateBuffer(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    dstBuffer,
		VkDeviceSize                                dstOffset,
		VkDeviceSize                                dataSize,
		const void*                                 pData) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<UpdateBufferCmd>();

	auto& buf = cb.dev->buffers.get(dstBuffer);
	auto dataPtr = static_cast<const std::byte*>(pData);
	cmd->data = {dataPtr, dataPtr + dataSize};
	cmd->dst = &buf;
	cmd->offset = dstOffset;

	useBuffer(cb, *cmd, buf);

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdUpdateBuffer(commandBuffer, dstBuffer, dstOffset, dataSize, pData);
}

VKAPI_ATTR void VKAPI_CALL CmdFillBuffer(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    dstBuffer,
		VkDeviceSize                                dstOffset,
		VkDeviceSize                                size,
		uint32_t                                    data) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<FillBufferCmd>();

	auto& buf = cb.dev->buffers.get(dstBuffer);
	cmd->dst = &buf;
	cmd->offset = dstOffset;
	cmd->size = size;
	cmd->data = data;

	useBuffer(cb, *cmd, buf);

	cb.commands.push_back(std::move(cmd));
	cb.dev->dispatch.vkCmdFillBuffer(commandBuffer, dstBuffer, dstOffset, size, data);
}

} // namespace fuen
