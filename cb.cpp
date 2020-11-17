#include "cb.hpp"
#include "data.hpp"
#include "rp.hpp"
#include "util.hpp"
#include "gui.hpp"
#include "ds.hpp"
#include "buffer.hpp"
#include "commands.hpp"
#include "pipe.hpp"
#include "image.hpp"
#include "imgui/imgui.h"
#include <vkpp/names.hpp>

namespace fuen {

// util
void reset(CommandBuffer& cb) {
	std::lock_guard lock(cb.mutex);
	cb.buffers.clear();
	cb.images.clear();
	cb.commands.clear();
	cb.sections.clear();
	cb.graphicsState = {};
	cb.computeState = {};
	cb.pushConstants = {};
	++cb.resetCount;
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
	auto cp = dev.commandPools.mustMove(commandPool);

	for(auto* cb : cp->cbs) {
		eraseData(cb->handle);
		dev.commandBuffers.mustErase(cb->handle);
	}

	{
		// ~DeviceHandle expects device mutex locked
		std::lock_guard lock(dev.mutex);
		cp.reset();
	}

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
		cb.handle = pCommandBuffers[i];
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

	for(auto i = 0u; i < commandBufferCount; ++i) {
		auto cb = dev.commandBuffers.mustMove(pCommandBuffers[i]);
		if(cb->pool) {
			dlg_assert(cb->pool == dev.commandPools.find(commandPool));
			auto it = std::find(cb->pool->cbs.begin(), cb->pool->cbs.end(), cb.get());
			dlg_assert(it != cb->pool->cbs.end());
			cb->pool->cbs.erase(it);
		}

		eraseData(pCommandBuffers[i]);

		// ~DeviceHandle expects device mutex to be locked
		std::lock_guard lock(dev.mutex);
		cb.reset();
	}

	dev.dispatch.vkFreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
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
	dlg_assert(cb.sections.empty());
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

void add(CommandBuffer& cb, std::unique_ptr<Command> cmd) {
	std::lock_guard lock(cb.mutex);
	if(!cb.sections.empty()) {
		cb.sections.back()->children.emplace_back(std::move(cmd));
	} else {
		cb.commands.emplace_back(std::move(cmd));
	}
}

void addSection(CommandBuffer& cb, std::unique_ptr<SectionCommand> cmd) {
	auto* section = cmd.get();
	add(cb, std::move(cmd));
	cb.sections.push_back(section);
}

void addNextSection(CommandBuffer& cb, std::unique_ptr<SectionCommand> cmd) {
	auto* section = cmd.get();
	add(cb, std::move(cmd));
	dlg_assert(!cb.sections.empty());
	cb.sections.pop_back();
	cb.sections.push_back(section);
}

void addEndSection(CommandBuffer& cb, std::unique_ptr<Command> cmd) {
	add(cb, std::move(cmd));
	dlg_assert(!cb.sections.empty());
	cb.sections.pop_back();
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

	add(cb, std::move(cmd));
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

	add(cb, std::move(cmd));
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

	addSection(cb, std::move(cmd));
	cb.dev->dispatch.vkCmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
}

VKAPI_ATTR void VKAPI_CALL CmdNextSubpass(
		VkCommandBuffer                             commandBuffer,
		VkSubpassContents                           contents) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<NextSubpassCmd>();
	// TODO; figure this out, should subpass be whole section?
	// but then how to handle first subpass?
	// addNextSection(cb, std::move(cmd));
	add(cb, std::move(cmd));
	cb.dev->dispatch.vkCmdNextSubpass(commandBuffer, contents);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(
		VkCommandBuffer                             commandBuffer) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<EndRenderPassCmd>();
	addEndSection(cb, std::move(cmd));
	cb.dev->dispatch.vkCmdEndRenderPass(commandBuffer);
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
	cmd->pipeLayout = &cb.dev->pipeLayouts.get(layout);

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

	// TODO: not sure about this. The spec isn't clear about this.
	// But this seems to be what the validation layers do.
	// https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/57f6f2a387b37c442c4db6993eb064a1e750b30f/layers/state_tracker.cpp#L5868
	if(cb.pushConstants.layout &&
			pushConstantCompatible(*cmd->pipeLayout, *cb.pushConstants.layout)) {
		cb.pushConstants.layout = nullptr;
		cb.pushConstants.map.clear();
	}

	// update bound state
	if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
		cb.computeState.bind(*cmd->pipeLayout, firstSet, cmd->sets,
			{pDynamicOffsets, pDynamicOffsets + dynamicOffsetCount});
	} else if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
		cb.graphicsState.bind(*cmd->pipeLayout, firstSet, cmd->sets,
			{pDynamicOffsets, pDynamicOffsets + dynamicOffsetCount});
	} else {
		dlg_error("Unknown pipeline bind point");
	}

	add(cb, std::move(cmd));
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

	cb.graphicsState.indices.buffer = &buf;
	cb.graphicsState.indices.offset = offset;
	cb.graphicsState.indices.type = indexType;

	add(cb, std::move(cmd));
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
	cmd->firstBinding = firstBinding;

	ensureSize(cb.graphicsState.vertices, firstBinding + bindingCount);
	for(auto i = 0u; i < bindingCount; ++i) {
		auto& buf = cb.dev->buffers.get(pBuffers[i]);
		cmd->buffers.push_back(&buf);
		useBuffer(cb, *cmd, buf);

		cb.graphicsState.vertices[firstBinding + i].buffer = &buf;
		cb.graphicsState.vertices[firstBinding + i].offset = pOffsets[i];
	}

	add(cb, std::move(cmd));
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

	cmd->state = cb.graphicsState;
	if(cb.pushConstants.layout && pushConstantCompatible(*cmd->state.pipe->layout, *cb.pushConstants.layout)) {
		cmd->pushConstants = cb.pushConstants.map;
	}

	add(cb, std::move(cmd));
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

	cmd->state = cb.graphicsState;
	if(cb.pushConstants.layout && pushConstantCompatible(*cmd->state.pipe->layout, *cb.pushConstants.layout)) {
		cmd->pushConstants = cb.pushConstants.map;
	}

	add(cb, std::move(cmd));
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

	cmd->state = cb.graphicsState;
	if(cb.pushConstants.layout && pushConstantCompatible(*cmd->state.pipe->layout, *cb.pushConstants.layout)) {
		cmd->pushConstants = cb.pushConstants.map;
	}

	add(cb, std::move(cmd));
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

	cmd->state = cb.graphicsState;
	if(cb.pushConstants.layout && pushConstantCompatible(*cmd->state.pipe->layout, *cb.pushConstants.layout)) {
		cmd->pushConstants = cb.pushConstants.map;
	}

	add(cb, std::move(cmd));
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

	cmd->state = cb.computeState;
	if(cb.pushConstants.layout && pushConstantCompatible(*cmd->state.pipe->layout, *cb.pushConstants.layout)) {
		cmd->pushConstants = cb.pushConstants.map;
	}

	add(cb, std::move(cmd));
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

	cmd->state = cb.computeState;
	if(cb.pushConstants.layout && pushConstantCompatible(*cmd->state.pipe->layout, *cb.pushConstants.layout)) {
		cmd->pushConstants = cb.pushConstants.map;
	}

	add(cb, std::move(cmd));
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

	add(cb, std::move(cmd));
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

	add(cb, std::move(cmd));
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

	add(cb, std::move(cmd));
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

	add(cb, std::move(cmd));
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

	add(cb, std::move(cmd));
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

	add(cb, std::move(cmd));
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

	add(cb, std::move(cmd));
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

	add(cb, std::move(cmd));
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

	add(cb, std::move(cmd));
	cb.dev->dispatch.vkCmdFillBuffer(commandBuffer, dstBuffer, dstOffset, size, data);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginDebugUtilsLabelEXT(
		VkCommandBuffer                             commandBuffer,
		const VkDebugUtilsLabelEXT*                 pLabelInfo) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<BeginDebugUtilsLabelCmd>();

	auto* c = pLabelInfo->color;
	cmd->color = {c[0], c[1], c[2], c[3]};
	cmd->name = pLabelInfo->pLabelName;

	addSection(cb, std::move(cmd));

	if(cb.dev->dispatch.vkCmdBeginDebugUtilsLabelEXT) {
		cb.dev->dispatch.vkCmdBeginDebugUtilsLabelEXT(commandBuffer, pLabelInfo);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdEndDebugUtilsLabelEXT(
		VkCommandBuffer                             commandBuffer) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<EndDebugUtilsLabelCmd>();
	addEndSection(cb, std::move(cmd));

	if(cb.dev->dispatch.vkCmdEndDebugUtilsLabelEXT) {
		cb.dev->dispatch.vkCmdEndDebugUtilsLabelEXT(commandBuffer);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(
		VkCommandBuffer                             commandBuffer,
		VkPipelineBindPoint                         pipelineBindPoint,
		VkPipeline                                  pipeline) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<BindPipelineCmd>();
	cmd->bindPoint = pipelineBindPoint;

	if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
		cb.computeState.pipe = &cb.dev->computePipes.get(pipeline);
		cmd->pipe = cb.computeState.pipe;
	} else if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
		cb.graphicsState.pipe = &cb.dev->graphicsPipes.get(pipeline);
		cmd->pipe = cb.graphicsState.pipe;
	} else {
		dlg_error("unknown pipeline bind point");
	}

	add(cb, std::move(cmd));
	cb.dev->dispatch.vkCmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
}

VKAPI_ATTR void VKAPI_CALL CmdPushConstants(
		VkCommandBuffer                             commandBuffer,
		VkPipelineLayout                            layout,
		VkShaderStageFlags                          stageFlags,
		uint32_t                                    offset,
		uint32_t                                    size,
		const void*                                 pValues) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<PushConstantsCmd>();

	cmd->layout = &cb.dev->pipeLayouts.get(layout);
	cmd->stages = stageFlags;
	cmd->offset = offset;
	cmd->size = size;
	auto ptr = static_cast<const std::byte*>(pValues);
	cmd->values = {ptr, ptr + size};

	// TODO: not sure about this. The spec isn't clear about this.
	// But this seems to be what the validation layers do.
	// https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/57f6f2a387b37c442c4db6993eb064a1e750b30f/layers/state_tracker.cpp#L5868
	if(cb.pushConstants.layout &&
			pushConstantCompatible(*cmd->layout, *cb.pushConstants.layout)) {
		cb.pushConstants.layout = nullptr;
		cb.pushConstants.map.clear();
	}

	cb.pushConstants.layout = cmd->layout;
	for(auto i = 0u; i < 32; ++i) {
		if((stageFlags & (1 << i)) == 0) {
			continue;
		}

		auto& pc = cb.pushConstants.map[VkShaderStageFlagBits(1 << i)];
		ensureSize(pc.data, offset + size);
		std::memcpy(pc.data.data() + offset, pValues, size);

		auto it = pc.ranges.begin();
		for(; it != pc.ranges.end(); ++it) {
			if(it->first < offset) {
				continue;
			} else if(it->first == offset) {
				if(it->second < size) {
					it->second = size;
				}
			} else if(it->first + it->second == offset) {
				it->second += size;
			} else if(it->first > offset) {
				it = pc.ranges.insert(it, {offset, size});
			}

			// merge following ranges
			for(auto iit = ++it; it != pc.ranges.end();) {
				if(iit->first > it->first + it->second) {
					break;
				}

				it->second = std::max(it->second, iit->first + iit->second - offset);
				iit = pc.ranges.erase(iit);
			}

			break;
		}

		if(it == pc.ranges.end()) {
			pc.ranges.push_back({offset, size});
		}
	}

	add(cb, std::move(cmd));
	cb.dev->dispatch.vkCmdPushConstants(commandBuffer, layout, stageFlags,
		offset, size, pValues);
}

// util
void DescriptorState::bind(PipelineLayout& layout, u32 firstSet,
		span<DescriptorSet* const> sets, span<const u32>) {
	ensureSize(descriptorSets, firstSet + sets.size());

	// TODO: the "ds disturbing" part of vulkan is hard to grasp IMO.
	// There may be errors here.

	auto lastSet = firstSet + sets.size() - 1;
	for(auto i = 0u; i < firstSet; ++i) {
		if(!descriptorSets[i].ds) {
			continue;
		}

		dlg_assert(descriptorSets[i].layout);
		if(!compatibleForSetN(*descriptorSets[i].layout, layout, lastSet)) {
			// disturbed!
			descriptorSets[i] = {};
		}
	}

	// bind descriptors and check if future bindings are disturbed
	auto followingDisturbed = false;
	for(auto i = 0u; i < sets.size(); ++i) {
		auto s = firstSet + i;
		if(!descriptorSets[s].ds || !compatibleForSetN(*descriptorSets[s].layout, layout, s)) {
			followingDisturbed = true;
		}

		descriptorSets[s].layout = &layout;
		descriptorSets[s].ds = sets[i];
		// TODO: use given offsets. We have to analyze the layout and
		// count the offset into the offsets array.
		descriptorSets[s].dynamicOffsets = {};
	}

	if(followingDisturbed) {
		descriptorSets.resize(lastSet + 1);
	}
}

} // namespace fuen
