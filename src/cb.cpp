#include <cb.hpp>
#include <data.hpp>
#include <rp.hpp>
#include <util.hpp>
#include <ds.hpp>
#include <buffer.hpp>
#include <commands.hpp>
#include <pipe.hpp>
#include <image.hpp>

namespace fuen {

// util
void removeFromHandlesLocked(CommandBuffer& cb) {
	// remove cb from all referenced resources
	auto removeFromResource = [&](auto& res) {
		// We have to lock the resource mutex since other commands buffers
		// might add/remove them at the same time.
		auto it = find(res.refCbs, &cb);
		dlg_assert(it != res.refCbs.end());
		res.refCbs.erase(it);
	};

	for(auto& img : cb.images) {
		removeFromResource(*img.second.image);
	}

	for(auto& buf : cb.buffers) {
		removeFromResource(*buf.second.buffer);
	}

	for(auto& handle : cb.handles) {
		removeFromResource(*handle.second.handle);
	}
}

void resetLocked(CommandBuffer& cb, bool invalidate = false) {
	// make sure all submissions are done.
	for(auto* subm : cb.pending) {
		auto res = checkLocked(*subm);
		dlg_assert(res);
	}

	removeFromHandlesLocked(cb);

	// We have to lock our own mutex since other threads might read
	// our data at the same time.
	cb.state = invalidate ?
		CommandBuffer::State::invalid :
		CommandBuffer::State::initial;
	cb.buffers.clear();
	cb.images.clear();
	cb.handles.clear();
	cb.commands.clear();
	cb.sections.clear();
	cb.graphicsState = {};
	cb.computeState = {};
	cb.pushConstants = {};

	if(!invalidate) {
		++cb.resetCount;
	}
}

void reset(CommandBuffer& cb, bool invalidate = false) {
	std::lock_guard lock(cb.dev->mutex);
	resetLocked(cb, invalidate);
}

void CommandBuffer::invalidateLocked() {
	resetLocked(*this, true);
}

CommandBuffer::~CommandBuffer() {
	if(!dev) {
		return;
	}

	std::lock_guard devLock(this->dev->mutex);

	// make sure all submissions are done.
	for(auto* subm : this->pending) {
		auto res = checkLocked(*subm);
		dlg_assert(res);
	}

	removeFromHandlesLocked(*this);

	// Remove ourselves from the pool we come from.
	// A command pool can't be destroyed before its command buffers (it
	// implicitly frees them).
	dlg_assert(pool);

	auto it = find(pool->cbs, this);
	dlg_assert(it != pool->cbs.end());
	pool->cbs.erase(it);
}

CommandPool::~CommandPool() {
	if(!dev) {
		return;
	}

	// NOTE: we don't need a lock here:
	// While the command pool is being destroyed, no command buffers from it
	// can be created or destroyed in another thread, that would always be a
	// race. So accessing this vector is safe.
	// (Just adding a lock here would furthermore result in deadlocks due
	// to the mutexes locked inside the loop, don't do it!)
	// We don't use a for loop since the command buffers remove themselves
	// on destruction
	while(!cbs.empty()) {
		auto* cb = cbs[0];
		eraseData(cb->handle);
		dev->commandBuffers.mustErase(cb->handle);
	}
}

// api
// command pool
VKAPI_ATTR VkResult VKAPI_CALL CreateCommandPool(
		VkDevice                                    device,
		const VkCommandPoolCreateInfo*              pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkCommandPool*                              pCommandPool) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.CreateCommandPool(device, pCreateInfo, pAllocator, pCommandPool);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& cp = dev.commandPools.add(*pCommandPool);
	cp.objectType = VK_OBJECT_TYPE_COMMAND_POOL;
	cp.dev = &dev;
	cp.handle = *pCommandPool;
	cp.queueFamily = pCreateInfo->queueFamilyIndex;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(
		VkDevice                                    device,
		VkCommandPool                               commandPool,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.commandPools.mustErase(commandPool);
	dev.dispatch.DestroyCommandPool(device, commandPool, pAllocator);
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
	return dev.dispatch.ResetCommandPool(device, commandPool, flags);
}

// command buffer
VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(
		VkDevice                                    device,
		const VkCommandBufferAllocateInfo*          pAllocateInfo,
		VkCommandBuffer*                            pCommandBuffers) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto* cp = dev.commandPools.find(pAllocateInfo->commandPool);
	for(auto i = 0u; i < pAllocateInfo->commandBufferCount; ++i) {
		auto& cb = dev.commandBuffers.add(pCommandBuffers[i]);
		cb.dev = &dev;
		cb.objectType = VK_OBJECT_TYPE_COMMAND_BUFFER;
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
		eraseData(pCommandBuffers[i]);
		dev.commandBuffers.mustErase(pCommandBuffers[i]);
	}

	dev.dispatch.FreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(
		VkCommandBuffer                             commandBuffer,
		const VkCommandBufferBeginInfo*             pBeginInfo) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	reset(cb);
	return cb.dev->dispatch.BeginCommandBuffer(commandBuffer, pBeginInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL EndCommandBuffer(
		VkCommandBuffer                             commandBuffer) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	dlg_assert(cb.sections.empty()); // all sections must have been popped

	{
		std::lock_guard lock(cb.dev->mutex);
		cb.state = CommandBuffer::State::executable;
	}

	return cb.dev->dispatch.EndCommandBuffer(commandBuffer);
}

VKAPI_ATTR VkResult VKAPI_CALL ResetCommandBuffer(
		VkCommandBuffer                             commandBuffer,
		VkCommandBufferResetFlags                   flags) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	reset(cb);
	return cb.dev->dispatch.ResetCommandBuffer(commandBuffer, flags);
}

// == command buffer recording ==
// util
void addToHandle(CommandBuffer& cb, DeviceHandle& handle) {
	std::lock_guard lock(cb.dev->mutex);
	dlg_assert(std::find(handle.refCbs.begin(), handle.refCbs.end(), &cb) == handle.refCbs.end());
	handle.refCbs.push_back(&cb);
}

CommandBuffer::UsedImage& useImage(CommandBuffer& cb, Command& cmd, Image& image) {
	auto& img = cb.images[image.handle];
	img.commands.push_back(&cmd);
	if(!img.image) {
		img.image = &image;
		dlg_assert(img.image);
		addToHandle(cb, image);
	}

	return img;
}

CommandBuffer::UsedImage& useImage(CommandBuffer& cb, Command& cmd, VkImage image) {
	auto& img = cb.images[image];
	img.commands.push_back(&cmd);
	if(!img.image) {
		img.image = &cb.dev->images.get(image);
		addToHandle(cb, *img.image);
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
	if(!useBuf.buffer) {
		useBuf.buffer = &buf;
		addToHandle(cb, buf);
	}

	useBuf.commands.push_back(&cmd);
	return useBuf;
}

void useHandle(CommandBuffer& cb, Command& cmd, std::uint64_t h64, DeviceHandle& handle) {
	auto& uh = cb.handles[h64];
	if(!uh.handle) {
		uh.handle = &handle;
		addToHandle(cb, *uh.handle);
	}

	uh.commands.push_back(&cmd);
}

template<typename T>
void useHandle(CommandBuffer& cb, Command& cmd, T& handle) {
	auto h64 = handleToU64(handle.handle);
	useHandle(cb, cmd, h64, handle);
}

void add(CommandBuffer& cb, std::unique_ptr<Command> cmd) {
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
		imgb.pNext = nullptr;
		auto& img = cb.dev->images.get(imgb.image);
		// cmd.images.push_back(&img);
		useImage(cb, cmd, img, imgb.newLayout);
	}

	for(auto& buf : cmd.bufBarriers) {
		buf.pNext = nullptr;
		auto& bbuf = cb.dev->buffers.get(buf.buffer);
		// cmd.buffers.push_back(&bbuf);
		useBuffer(cb, cmd, bbuf);
	}

	for(auto& mem : cmd.memBarriers) {
		mem.pNext = nullptr;
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

	for(auto i = 0u; i < eventCount; ++i) {
		auto& event = cb.dev->events.get(pEvents[i]);
		useHandle(cb, *cmd, event);
	}

	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdWaitEvents(commandBuffer, eventCount, pEvents,
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
	cb.dev->dispatch.CmdPipelineBarrier(commandBuffer,
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
	cmd->clearValues = {
		pRenderPassBegin->pClearValues,
		pRenderPassBegin->pClearValues + pRenderPassBegin->clearValueCount
	};
	cmd->info.pNext = nullptr;
	cmd->info.pClearValues = cmd->clearValues.data();

	cmd->subpassContents = contents;
	cmd->fb = cb.dev->framebuffers.find(pRenderPassBegin->framebuffer);
	cmd->rp = cb.dev->renderPasses.find(pRenderPassBegin->renderPass);

	dlg_assert(cmd->fb);
	dlg_assert(cmd->rp);

	useHandle(cb, *cmd, *cmd->fb);
	useHandle(cb, *cmd, *cmd->rp);

	if(cmd->fb && cmd->rp) {
		dlg_assert(cmd->rp->desc->attachments.size() == cmd->fb->attachments.size());
		for(auto i = 0u; i < cmd->fb->attachments.size(); ++i) {
			auto& attachment = cmd->fb->attachments[i];
			if(!attachment || !attachment->img) {
				continue;
			}

			// TODO: can there be a transition inside the renderpass on
			//   an attachment? probably not...
			//   maybe better move this to RenderPassEnd nonetheless?
			// TODO: handle secondary command buffers and stuff
			useHandle(cb, *cmd, *attachment);
			useImage(cb, *cmd, *attachment->img,
				cmd->rp->desc->attachments[i].finalLayout);
		}
	}

	addSection(cb, std::move(cmd));
	cb.dev->dispatch.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
}

VKAPI_ATTR void VKAPI_CALL CmdNextSubpass(
		VkCommandBuffer                             commandBuffer,
		VkSubpassContents                           contents) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<NextSubpassCmd>();
	cmd->subpassContents = contents;
	// TODO; figure this out, should subpass be whole section?
	// but then how to handle first subpass?
	// addNextSection(cb, std::move(cmd));
	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdNextSubpass(commandBuffer, contents);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(
		VkCommandBuffer                             commandBuffer) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<EndRenderPassCmd>();
	addEndSection(cb, std::move(cmd));
	cb.dev->dispatch.CmdEndRenderPass(commandBuffer);
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
	cmd->dynamicOffsets = {pDynamicOffsets, pDynamicOffsets + dynamicOffsetCount};

	// NOTE: the pipeline layout is intetionally not added to used handles
	// since the application not destroying it does not move the command
	// buffer into invalid state (and vulkan requires that it's kept
	// alive while recording).
	// Since we might need it lateron, we acquire shared ownership.
	// Also like this in CmdPushConstants
	cmd->pipeLayout = cb.dev->pipeLayouts.getPtr(layout);

	for(auto i = 0u; i < descriptorSetCount; ++i) {
		auto& ds = cb.dev->descriptorSets.get(pDescriptorSets[i]);

		for(auto b = 0u; b < ds.bindings.size(); ++b) {
			auto cat = category(ds.layout->bindings[b].descriptorType);
			if(cat == DescriptorCategory::image) {
				for(auto e = 0u; e < ds.bindings[b].size(); ++e) {
					if(!ds.bindings[b][e].valid || !ds.bindings[b][e].imageInfo.imageView) {
						continue;
					}

					auto* view = ds.bindings[b][e].imageInfo.imageView;
					dlg_assert(view);
					useImage(cb, *cmd, *view->img);
				}
			} else if(cat == DescriptorCategory::buffer) {
				for(auto e = 0u; e < ds.bindings[b].size(); ++e) {
					if(!ds.bindings[b][e].valid || !ds.bindings[b][e].bufferInfo.buffer) {
						continue;
					}

					auto* buf = ds.bindings[b][e].bufferInfo.buffer;
					dlg_assert(buf);
					useBuffer(cb, *cmd, *buf);
				}
			} // TODO: buffer view
		}

		useHandle(cb, *cmd, ds);
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
	cb.dev->dispatch.CmdBindDescriptorSets(commandBuffer,
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

	cmd->offset = offset;
	cmd->indexType = indexType;

	cb.graphicsState.indices.buffer = &buf;
	cb.graphicsState.indices.offset = offset;
	cb.graphicsState.indices.type = indexType;

	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdBindIndexBuffer(commandBuffer,
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
	cmd->offsets = {pOffsets, pOffsets + bindingCount};

	ensureSize(cb.graphicsState.vertices, firstBinding + bindingCount);
	for(auto i = 0u; i < bindingCount; ++i) {
		auto& buf = cb.dev->buffers.get(pBuffers[i]);
		cmd->buffers.push_back(&buf);
		useBuffer(cb, *cmd, buf);

		cb.graphicsState.vertices[firstBinding + i].buffer = &buf;
		cb.graphicsState.vertices[firstBinding + i].offset = pOffsets[i];
	}

	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdBindVertexBuffers(commandBuffer,
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
	cmd->state.pushConstants = cb.pushConstants.map;
	if(cb.pushConstants.layout && pushConstantCompatible(*cmd->state.pipe->layout, *cb.pushConstants.layout)) {
		cmd->pushConstants = cb.pushConstants.map;
	}

	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdDraw(commandBuffer,
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
	cmd->state.pushConstants = cb.pushConstants.map;
	if(cb.pushConstants.layout && pushConstantCompatible(*cmd->state.pipe->layout, *cb.pushConstants.layout)) {
		cmd->pushConstants = cb.pushConstants.map;
	}

	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdDrawIndexed(commandBuffer,
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

	cmd->offset = offset;
	cmd->drawCount = drawCount;
	cmd->stride = stride;
	cmd->state = cb.graphicsState;
	cmd->state.pushConstants = cb.pushConstants.map;
	if(cb.pushConstants.layout && pushConstantCompatible(*cmd->state.pipe->layout, *cb.pushConstants.layout)) {
		cmd->pushConstants = cb.pushConstants.map;
	}

	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdDrawIndirect(commandBuffer,
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

	cmd->offset = offset;
	cmd->drawCount = drawCount;
	cmd->stride = stride;
	cmd->state = cb.graphicsState;
	cmd->state.pushConstants = cb.pushConstants.map;
	if(cb.pushConstants.layout && pushConstantCompatible(*cmd->state.pipe->layout, *cb.pushConstants.layout)) {
		cmd->pushConstants = cb.pushConstants.map;
	}

	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdDrawIndexedIndirect(commandBuffer,
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
	cmd->state.pushConstants = cb.pushConstants.map;
	if(cb.pushConstants.layout && pushConstantCompatible(*cmd->state.pipe->layout, *cb.pushConstants.layout)) {
		cmd->pushConstants = cb.pushConstants.map;
	}

	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdDispatch(commandBuffer,
		groupCountX, groupCountY, groupCountZ);
}

VKAPI_ATTR void VKAPI_CALL CmdDispatchIndirect(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    buffer,
		VkDeviceSize                                offset) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<DispatchIndirectCmd>();
	cmd->offset = offset;

	auto& buf = cb.dev->buffers.get(buffer);
	cmd->buffer = &buf;
	useBuffer(cb, *cmd, buf);

	cmd->state = cb.computeState;
	cmd->state.pushConstants = cb.pushConstants.map;
	if(cb.pushConstants.layout && pushConstantCompatible(*cmd->state.pipe->layout, *cb.pushConstants.layout)) {
		cmd->pushConstants = cb.pushConstants.map;
	}

	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdDispatchIndirect(commandBuffer, buffer, offset);
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
	cmd->srcLayout = srcImageLayout;
	cmd->dst = &dst;
	cmd->dstLayout = dstImageLayout;
	cmd->copies = {pRegions, pRegions + regionCount};

	useImage(cb, *cmd, src);
	useImage(cb, *cmd, dst);

	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdCopyImage(commandBuffer,
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
	cmd->srcLayout = srcImageLayout;
	cmd->dst = &dst;
	cmd->dstLayout = dstImageLayout;
	cmd->blits = {pRegions, pRegions + regionCount};
	cmd->filter = filter;

	useImage(cb, *cmd, src);
	useImage(cb, *cmd, dst);

	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdBlitImage(commandBuffer,
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
	cmd->imgLayout = dstImageLayout;
	cmd->copies = {pRegions, pRegions + regionCount};

	useBuffer(cb, *cmd, src);
	useImage(cb, *cmd, dst);

	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdCopyBufferToImage(commandBuffer,
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
	cmd->imgLayout = srcImageLayout;
	cmd->copies = {pRegions, pRegions + regionCount};

	useImage(cb, *cmd, src);
	useBuffer(cb, *cmd, dst);

	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdCopyImageToBuffer(commandBuffer,
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
	cmd->color = *pColor;
	cmd->imgLayout = imageLayout;
	cmd->ranges = {pRanges, pRanges + rangeCount};

	useImage(cb, *cmd, dst);

	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdClearColorImage(commandBuffer,
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
		useHandle(cb, *cmd, secondary);

		// TODO: really track all of them here?
		// TODO: also add the subcommands to the commands array??
		for(auto& img : secondary.images) {
			if(img.second.layoutChanged) {
				useImage(cb, *cmd, *img.second.image, img.second.finalLayout);
			} else {
				useImage(cb, *cmd, *img.second.image);
			}
		}

		for(auto& buf : secondary.buffers) {
			useBuffer(cb, *cmd, *buf.second.buffer);
		}

		for(auto& handle : secondary.handles) {
			useHandle(cb, *cmd, handle.first, *handle.second.handle);
		}
	}

	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdExecuteCommands(commandBuffer,
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
	cb.dev->dispatch.CmdCopyBuffer(commandBuffer,
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
	cb.dev->dispatch.CmdUpdateBuffer(commandBuffer, dstBuffer, dstOffset, dataSize, pData);
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
	cb.dev->dispatch.CmdFillBuffer(commandBuffer, dstBuffer, dstOffset, size, data);
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

	if(cb.dev->dispatch.CmdBeginDebugUtilsLabelEXT) {
		cb.dev->dispatch.CmdBeginDebugUtilsLabelEXT(commandBuffer, pLabelInfo);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdEndDebugUtilsLabelEXT(
		VkCommandBuffer                             commandBuffer) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<EndDebugUtilsLabelCmd>();
	addEndSection(cb, std::move(cmd));

	if(cb.dev->dispatch.CmdEndDebugUtilsLabelEXT) {
		cb.dev->dispatch.CmdEndDebugUtilsLabelEXT(commandBuffer);
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

	useHandle(cb, *cmd, *cmd->pipe);

	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
}

VKAPI_ATTR void VKAPI_CALL CmdPushConstants(
		VkCommandBuffer                             commandBuffer,
		VkPipelineLayout                            pipeLayout,
		VkShaderStageFlags                          stageFlags,
		uint32_t                                    offset,
		uint32_t                                    size,
		const void*                                 pValues) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<PushConstantsCmd>();

	// NOTE: See BindDescriptorSets for rationale on handling here.
	cmd->layout = cb.dev->pipeLayouts.getPtr(pipeLayout);

	cmd->stages = stageFlags;
	cmd->offset = offset;
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

	cb.pushConstants.layout = cmd->layout.get();
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
	cb.dev->dispatch.CmdPushConstants(commandBuffer, pipeLayout, stageFlags,
		offset, size, pValues);
}

VKAPI_ATTR void VKAPI_CALL CmdSetViewport(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    firstViewport,
		uint32_t                                    viewportCount,
		const VkViewport*                           pViewports) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<SetViewportCmd>();
	cmd->first = firstViewport;
	cmd->viewports = {pViewports, pViewports + viewportCount};
	add(cb, std::move(cmd));

	ensureSize(cb.graphicsState.dynamic.viewports, firstViewport + viewportCount);
	std::copy(pViewports, pViewports + viewportCount,
		cb.graphicsState.dynamic.viewports.begin() + firstViewport);

	cb.dev->dispatch.CmdSetViewport(commandBuffer, firstViewport, viewportCount, pViewports);
}

VKAPI_ATTR void VKAPI_CALL CmdSetScissor(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    firstScissor,
		uint32_t                                    scissorCount,
		const VkRect2D*                             pScissors) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<SetScissorCmd>();
	cmd->first = firstScissor;
	cmd->scissors = {pScissors, pScissors + scissorCount};
	add(cb, std::move(cmd));

	ensureSize(cb.graphicsState.dynamic.scissors, firstScissor + scissorCount);
	std::copy(pScissors, pScissors + scissorCount,
		cb.graphicsState.dynamic.scissors.begin() + firstScissor);

	cb.dev->dispatch.CmdSetScissor(commandBuffer, firstScissor, scissorCount, pScissors);
}

VKAPI_ATTR void VKAPI_CALL CmdSetLineWidth(
		VkCommandBuffer                             commandBuffer,
		float                                       lineWidth) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<SetLineWidthCmd>();
	cmd->width = lineWidth;
	add(cb, std::move(cmd));
	cb.graphicsState.dynamic.lineWidth = lineWidth;
	cb.dev->dispatch.CmdSetLineWidth(commandBuffer, lineWidth);
}

VKAPI_ATTR void VKAPI_CALL CmdSetDepthBias(
		VkCommandBuffer                             commandBuffer,
		float                                       depthBiasConstantFactor,
		float                                       depthBiasClamp,
		float                                       depthBiasSlopeFactor) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<SetDepthBiasCmd>();
	cmd->state = {depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor};
	cb.graphicsState.dynamic.depthBias = cmd->state;
	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdSetDepthBias(commandBuffer,
		depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
}

VKAPI_ATTR void VKAPI_CALL CmdSetBlendConstants(
		VkCommandBuffer                             commandBuffer,
		const float                                 blendConstants[4]) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<SetBlendConstantsCmd>();
	std::memcpy(cmd->values.data(), blendConstants, sizeof(cmd->values));
	std::memcpy(cb.graphicsState.dynamic.blendConstants.data(), blendConstants,
		sizeof(cb.graphicsState.dynamic.blendConstants));
	add(cb, std::move(cmd));
	cb.dev->dispatch.CmdSetBlendConstants(commandBuffer, blendConstants);
}

VKAPI_ATTR void VKAPI_CALL CmdSetDepthBounds(
		VkCommandBuffer                             commandBuffer,
		float                                       minDepthBounds,
		float                                       maxDepthBounds) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<SetDepthBoundsCmd>();
	cmd->min = minDepthBounds;
	cmd->max = maxDepthBounds;
	add(cb, std::move(cmd));
	cb.graphicsState.dynamic.depthBoundsMin = minDepthBounds;
	cb.graphicsState.dynamic.depthBoundsMax = maxDepthBounds;
	cb.dev->dispatch.CmdSetDepthBounds(commandBuffer, minDepthBounds, maxDepthBounds);
}

VKAPI_ATTR void VKAPI_CALL CmdSetStencilCompareMask(
		VkCommandBuffer                             commandBuffer,
		VkStencilFaceFlags                          faceMask,
		uint32_t                                    compareMask) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<SetStencilCompareMaskCmd>();
	cmd->faceMask = faceMask;
	cmd->value = compareMask;
	add(cb, std::move(cmd));
	if(faceMask & VK_STENCIL_FACE_FRONT_BIT) {
		cb.graphicsState.dynamic.stencilFront.compareMask = compareMask;
	}
	if(faceMask & VK_STENCIL_FACE_BACK_BIT) {
		cb.graphicsState.dynamic.stencilBack.compareMask = compareMask;
	}
	cb.dev->dispatch.CmdSetStencilCompareMask(commandBuffer, faceMask, compareMask);
}

VKAPI_ATTR void VKAPI_CALL CmdSetStencilWriteMask(
		VkCommandBuffer                             commandBuffer,
		VkStencilFaceFlags                          faceMask,
		uint32_t                                    writeMask) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<SetStencilWriteMaskCmd>();
	cmd->faceMask = faceMask;
	cmd->value = writeMask;
	add(cb, std::move(cmd));
	if(faceMask & VK_STENCIL_FACE_FRONT_BIT) {
		cb.graphicsState.dynamic.stencilFront.compareMask = writeMask;
	}
	if(faceMask & VK_STENCIL_FACE_BACK_BIT) {
		cb.graphicsState.dynamic.stencilBack.compareMask = writeMask;
	}
	cb.dev->dispatch.CmdSetStencilWriteMask(commandBuffer, faceMask, writeMask);
}

VKAPI_ATTR void VKAPI_CALL CmdSetStencilReference(
		VkCommandBuffer                             commandBuffer,
		VkStencilFaceFlags                          faceMask,
		uint32_t                                    reference) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto cmd = std::make_unique<SetStencilReferenceCmd>();
	cmd->faceMask = faceMask;
	cmd->value = reference;
	add(cb, std::move(cmd));
	if(faceMask & VK_STENCIL_FACE_FRONT_BIT) {
		cb.graphicsState.dynamic.stencilFront.reference = reference;
	}
	if(faceMask & VK_STENCIL_FACE_BACK_BIT) {
		cb.graphicsState.dynamic.stencilBack.reference = reference;
	}
	cb.dev->dispatch.CmdSetStencilReference(commandBuffer, faceMask, reference);
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
