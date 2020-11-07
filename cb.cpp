 #include "cb.hpp"
 #include "data.hpp"
 #include "rp.hpp"
 #include "image.hpp"

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
};

struct BarrierCmd : BarrierCmdBase {
    VkDependencyFlags dependencyFlags;
};

struct BeginRenderPassCmd : BarrierCmdBase {
	VkRenderPassBeginInfo info;
	Framebuffer* fb;
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
		auto& img = cb.images[imgb.image];
		img.finalLayout = imgb.newLayout;
		if(!img.image) {
			img.image = cb.dev->images.find(imgb.image);
		}
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

	dlg_assert(cmd->fb);
	dlg_assert(cmd->fb && cmd->fb->rp);
	if(cmd->fb && cmd->fb->rp) {
		dlg_assert(cmd->fb->rp->info.attachments.size() == cmd->fb->attachments.size());
		for(auto i = 0u; i < cmd->fb->attachments.size(); ++i) {
			auto& attachment = cmd->fb->attachments[i];
			if(!attachment || !attachment->img) {
				continue;
			}

			// TODO: can there be barriers inside the renderpasss?
			//   maybe better move this to RenderPassEnd?
			// TODO: handle secondary command buffers.
			auto& img = cb.images[attachment->img->image];
			img.finalLayout = cmd->fb->rp->info.attachments[i].finalLayout;
			if(!img.image) {
				img.image = cb.dev->images.find(attachment->img->image);
			}
		}
	}

	cb.commands.push_back(std::move(cmd));

	cb.dev->dispatch.vkCmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
}

} // namespace fuen
