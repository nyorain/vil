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
	cb.commands.release();
	cb.sections.release();
	cb.graphicsState = {};
	cb.computeState = {};
	cb.pushConstants = {};

	// lock shouldn't really be needed for this btw
	dlg_assert(cb.pool);
	auto mb0 = std::make_move_iterator(cb.memBlocks.begin());
	auto mb1 = std::make_move_iterator(cb.memBlocks.end());
	cb.pool->memBlocks.insert(cb.pool->memBlocks.end(), mb0, mb1);
	cb.memBlocks.clear();
	cb.memBlockOffset = 0u;

	if(!invalidate) {
		++cb.resetCount;
	}
}

void reset(CommandBuffer& cb, bool invalidate = false) {
	std::lock_guard lock(cb.dev->mutex);
	resetLocked(cb, invalidate);
}

std::byte* allocate(CommandBuffer& cb, std::size_t size, std::size_t alignment) {
	dlg_assert(alignment <= size);
	dlg_assert((alignment & (alignment - 1)) == 0); // alignment must be power of two
	dlg_assert(alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__); // can't guarantee otherwise

	if(!cb.memBlocks.empty()) {
		cb.memBlockOffset = align(cb.memBlockOffset, alignment);
		auto remaining = i64(cb.memBlocks.back().size) - i64(cb.memBlockOffset);
		if(remaining > i64(size)) {
			return &cb.memBlocks.back().block[cb.memBlockOffset];
			cb.memBlockOffset += size;
		}
	}

	auto blockSize = std::max<std::size_t>(CommandPool::minBlockSize, size);
	if(!cb.pool->memBlocks.empty() && cb.pool->memBlocks.back().size >= size) {
		auto block = std::move(cb.pool->memBlocks.back());
		cb.pool->memBlocks.pop_back();
		cb.memBlocks.push_back(std::move(block));
	} else {
		cb.memBlocks.push_back({std::make_unique<std::byte[]>(blockSize), blockSize});
	}

	cb.memBlockOffset = size;
	return &cb.memBlocks.back().block[0];
}

enum class SectionType {
	none,
	begin,
	next,
	end,
};

template<typename T, SectionType ST = SectionType::none, typename... Args>
T& addCmd(CommandBuffer& cb, Args&&... args) {
	auto& cmd = allocate<T>(cb, std::forward<Args>(args)...);

	if(!cb.sections.empty()) {
		cmd.parent = cb.sections.back();
		cb.sections.back()->children.emplace_back(&cmd);
	} else {
		cb.commands.emplace_back(&cmd);
	}

	if constexpr(ST == SectionType::end || ST == SectionType::next) {
		dlg_assert(!cb.sections.empty());
		cb.sections.pop_back();
	}
	if constexpr(ST == SectionType::begin || ST == SectionType::next) {
		static_assert(std::is_convertible_v<T*, SectionCommand*>);
		cb.sections.push_back(&cmd);
	}

	return cmd;
}

CommandBuffer::CommandBuffer() : commands(*this), sections(*this) {
}

void CommandBuffer::invalidateLocked() {
	resetLocked(*this, true);
}

CommandBuffer::~CommandBuffer() {
	if(!dev) {
		return;
	}

	std::lock_guard devLock(this->dev->mutex);

	// Wait for completion, free all data and allocated stuff,
	// unregister from everything
	resetLocked(*this);

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
	if(!commandPool) {
		return;
	}

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

CommandBuffer::UsedImage& useHandle(CommandBuffer& cb, Command& cmd, Image& image) {
	auto& img = cb.images[image.handle];
	img.commands.push_back(&cmd);
	if(!img.image) {
		img.image = &image;
		dlg_assert(img.image);
		addToHandle(cb, image);
	}

	// TODO: add swapchain in case it's a swapchain image?
	dlg_assert(image.memory || image.swapchain);
	if(image.memory) {
		useHandle(cb, cmd, *image.memory);
	}

	return img;
}

void useHandle(CommandBuffer& cb, Command& cmd, ImageView& view, bool useImg = true) {
	auto h64 = handleToU64(view.handle);
	useHandle(cb, cmd, h64, view);

	dlg_assert(view.img);
	if(useImg && view.img) {
		useHandle(cb, cmd, *view.img);
	}
}

void useHandle(CommandBuffer& cb, Command& cmd, BufferView& view) {
	auto h64 = handleToU64(view.handle);
	useHandle(cb, cmd, h64, view);

	dlg_assert(view.buffer);
	if(view.buffer) {
		useHandle(cb, cmd, *view.buffer);
	}
}

CommandBuffer::UsedImage& useHandle(CommandBuffer& cb, Command& cmd, Image& image,
		VkImageLayout newLayout) {
	auto& img = useHandle(cb, cmd, image);
	img.layoutChanged = true;
	img.finalLayout = newLayout;
	return img;
}

CommandBuffer::UsedBuffer& useHandle(CommandBuffer& cb, Command& cmd, Buffer& buf) {
	auto& useBuf = cb.buffers[buf.handle];
	useBuf.commands.push_back(&cmd);
	if(!useBuf.buffer) {
		useBuf.buffer = &buf;
		addToHandle(cb, buf);
	}

	dlg_assert(buf.memory);
	if(buf.memory) {
		useHandle(cb, cmd, *buf.memory);
	}

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
		imgb.pNext = nullptr;
		auto& img = cb.dev->images.get(imgb.image);
		// cmd.images.push_back(&img);
		useHandle(cb, cmd, img, imgb.newLayout);
	}

	for(auto& buf : cmd.bufBarriers) {
		buf.pNext = nullptr;
		auto& bbuf = cb.dev->buffers.get(buf.buffer);
		// cmd.buffers.push_back(&bbuf);
		useHandle(cb, cmd, bbuf);
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
	auto& cmd = addCmd<WaitEventsCmd>(cb);
	cmdBarrier(cb, cmd, srcStageMask, dstStageMask,
		memoryBarrierCount, pMemoryBarriers,
		bufferMemoryBarrierCount, pBufferMemoryBarriers,
		imageMemoryBarrierCount, pImageMemoryBarriers);

	for(auto i = 0u; i < eventCount; ++i) {
		auto& event = cb.dev->events.get(pEvents[i]);
		cmd.events.push_back(&event);
		useHandle(cb, cmd, event);
	}

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
	auto& cmd = addCmd<BarrierCmd>(cb);
	cmd.dependencyFlags = dependencyFlags;
	cmdBarrier(cb, cmd, srcStageMask, dstStageMask,
		memoryBarrierCount, pMemoryBarriers,
		bufferMemoryBarrierCount, pBufferMemoryBarriers,
		imageMemoryBarrierCount, pImageMemoryBarriers);

	cb.dev->dispatch.CmdPipelineBarrier(commandBuffer,
		srcStageMask, dstStageMask, dependencyFlags,
		memoryBarrierCount, pMemoryBarriers,
		bufferMemoryBarrierCount, pBufferMemoryBarriers,
		imageMemoryBarrierCount, pImageMemoryBarriers);
}

template<typename BeginInfo>
void cmdBeginRenderPass(CommandBuffer& cb,
		const BeginInfo*               pRenderPassBegin,
		VkSubpassContents              contents) {
	auto& cmd = addCmd<BeginRenderPassCmd, SectionType::begin>(cb, cb);

	cmd.info = *pRenderPassBegin;
	cmd.clearValues = {
		pRenderPassBegin->pClearValues,
		pRenderPassBegin->pClearValues + pRenderPassBegin->clearValueCount
	};
	cmd.info.pNext = nullptr;
	cmd.info.pClearValues = cmd.clearValues.data();

	cmd.subpassContents = contents;
	cmd.fb = cb.dev->framebuffers.find(pRenderPassBegin->framebuffer);
	cmd.rp = cb.dev->renderPasses.find(pRenderPassBegin->renderPass);

	dlg_assert(cmd.fb);
	dlg_assert(cmd.rp);

	useHandle(cb, cmd, *cmd.fb);
	useHandle(cb, cmd, *cmd.rp);

	if(cmd.fb && cmd.rp) {
		dlg_assert(cmd.rp->desc->attachments.size() == cmd.fb->attachments.size());
		for(auto i = 0u; i < cmd.fb->attachments.size(); ++i) {
			auto& attachment = cmd.fb->attachments[i];
			if(!attachment || !attachment->img) {
				continue;
			}

			// TODO: can there be a transition inside the renderpass on
			//   an attachment? probably not...
			//   maybe better move this to RenderPassEnd nonetheless?
			// TODO: handle secondary command buffers and stuff
			useHandle(cb, cmd, *attachment, false);
			useHandle(cb, cmd, *attachment->img,
				cmd.rp->desc->attachments[i].finalLayout);
		}
	}
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(
		VkCommandBuffer                             commandBuffer,
		const VkRenderPassBeginInfo*                pRenderPassBegin,
		VkSubpassContents                           contents) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	cmdBeginRenderPass(cb, pRenderPassBegin, contents);
	cb.dev->dispatch.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
}

VKAPI_ATTR void VKAPI_CALL CmdNextSubpass(
		VkCommandBuffer                             commandBuffer,
		VkSubpassContents                           contents) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	// TODO; figure this out, should subpass be whole section?
	// but then how to handle first subpass?
	// addNextSection(cb, std::move(cmd));
	auto& cmd = addCmd<NextSubpassCmd, SectionType::next>(cb, cb);
	cmd.subpassContents = contents;
	cb.dev->dispatch.CmdNextSubpass(commandBuffer, contents);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(
		VkCommandBuffer                             commandBuffer) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<EndRenderPassCmd, SectionType::end>(cb);
	(void) cmd;
	cb.dev->dispatch.CmdEndRenderPass(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass2(
		VkCommandBuffer                             commandBuffer,
		const VkRenderPassBeginInfo*                pRenderPassBegin,
		const VkSubpassBeginInfo*                   pSubpassBeginInfo) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	cmdBeginRenderPass(cb, pRenderPassBegin, pSubpassBeginInfo->contents);
	cb.dev->dispatch.CmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
}

VKAPI_ATTR void VKAPI_CALL CmdNextSubpass2(
		VkCommandBuffer                             commandBuffer,
		const VkSubpassBeginInfo*                   pSubpassBeginInfo,
		const VkSubpassEndInfo*                     pSubpassEndInfo) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	// TODO; figure this out, should subpass be whole section?
	// but then how to handle first subpass?
	// addNextSection(cb, std::move(cmd));
	auto& cmd = addCmd<NextSubpassCmd, SectionType::next>(cb, cb);
	cmd.subpassContents = pSubpassBeginInfo->contents;
	cb.dev->dispatch.CmdNextSubpass2(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass2(
		VkCommandBuffer                             commandBuffer,
		const VkSubpassEndInfo*                     pSubpassEndInfo) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<EndRenderPassCmd, SectionType::end>(cb);
	(void) cmd;
	cb.dev->dispatch.CmdEndRenderPass2(commandBuffer, pSubpassEndInfo);
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
	auto& cmd = addCmd<BindDescriptorSetCmd>(cb);

	cmd.firstSet = firstSet;
	cmd.pipeBindPoint = pipelineBindPoint;
	cmd.dynamicOffsets = {pDynamicOffsets, pDynamicOffsets + dynamicOffsetCount};

	// NOTE: the pipeline layout is intetionally not added to used handles
	// since the application not destroying it does not move the command
	// buffer into invalid state (and vulkan requires that it's kept
	// alive while recording).
	// Since we might need it lateron, we acquire shared ownership.
	// Also like this in CmdPushConstants
	cmd.pipeLayout = cb.dev->pipeLayouts.getPtr(layout);

	for(auto i = 0u; i < descriptorSetCount; ++i) {
		auto& ds = cb.dev->descriptorSets.get(pDescriptorSets[i]);

		for(auto b = 0u; b < ds.bindings.size(); ++b) {
			auto cat = category(ds.layout->bindings[b].descriptorType);
			if(cat == DescriptorCategory::image) {
				for(auto e = 0u; e < ds.bindings[b].size(); ++e) {
					if(!ds.bindings[b][e].valid) {
						continue;
					}

					if(auto* view = ds.bindings[b][e].imageInfo.imageView; view) {
						useHandle(cb, cmd, *view);
					}

					if(auto* sampler = ds.bindings[b][e].imageInfo.sampler; sampler) {
						useHandle(cb, cmd, *sampler);
					}
				}
			} else if(cat == DescriptorCategory::buffer) {
				for(auto e = 0u; e < ds.bindings[b].size(); ++e) {
					if(!ds.bindings[b][e].valid || !ds.bindings[b][e].bufferInfo.buffer) {
						continue;
					}

					auto* buf = ds.bindings[b][e].bufferInfo.buffer;
					useHandle(cb, cmd, *buf);
				}
			} else if(cat == DescriptorCategory::bufferView) {
				for(auto e = 0u; e < ds.bindings[b].size(); ++e) {
					if(!ds.bindings[b][e].valid || !ds.bindings[b][e].bufferView) {
						continue;
					}

					auto* view = ds.bindings[b][e].bufferView;
					dlg_assert(view->buffer);
					useHandle(cb, cmd, *view);
				}
			}
		}

		useHandle(cb, cmd, ds);
		cmd.sets.push_back(&ds);
	}

	// TODO: not sure about this. The spec isn't clear about this.
	// But this seems to be what the validation layers do.
	// https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/57f6f2a387b37c442c4db6993eb064a1e750b30f/layers/state_tracker.cpp#L5868
	if(cb.pushConstants.layout &&
			pushConstantCompatible(*cmd.pipeLayout, *cb.pushConstants.layout)) {
		cb.pushConstants.layout = nullptr;
		cb.pushConstants.map.clear();
	}

	// update bound state
	if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
		cb.computeState.bind(*cmd.pipeLayout, firstSet, cmd.sets,
			{pDynamicOffsets, pDynamicOffsets + dynamicOffsetCount});
	} else if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
		cb.graphicsState.bind(*cmd.pipeLayout, firstSet, cmd.sets,
			{pDynamicOffsets, pDynamicOffsets + dynamicOffsetCount});
	} else {
		dlg_error("Unknown pipeline bind point");
	}

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
	auto& cmd = addCmd<BindIndexBufferCmd>(cb);

	auto& buf = cb.dev->buffers.get(buffer);
	cmd.buffer = &buf;
	useHandle(cb, cmd, buf);

	cmd.offset = offset;
	cmd.indexType = indexType;

	cb.graphicsState.indices.buffer = &buf;
	cb.graphicsState.indices.offset = offset;
	cb.graphicsState.indices.type = indexType;

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
	auto& cmd = addCmd<BindVertexBuffersCmd>(cb);
	cmd.firstBinding = firstBinding;
	cmd.offsets = {pOffsets, pOffsets + bindingCount};

	ensureSize(cb.graphicsState.vertices, firstBinding + bindingCount);
	for(auto i = 0u; i < bindingCount; ++i) {
		auto& buf = cb.dev->buffers.get(pBuffers[i]);
		cmd.buffers.push_back(&buf);
		useHandle(cb, cmd, buf);

		cb.graphicsState.vertices[firstBinding + i].buffer = &buf;
		cb.graphicsState.vertices[firstBinding + i].offset = pOffsets[i];
	}

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
	auto& cmd = addCmd<DrawCmd>(cb, cb);

	cmd.vertexCount = vertexCount;
	cmd.instanceCount = instanceCount;
	cmd.firstVertex = firstVertex;
	cmd.firstInstance = firstInstance;

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
	auto& cmd = addCmd<DrawIndexedCmd>(cb, cb);

	cmd.firstInstance = firstInstance;
	cmd.instanceCount = instanceCount;
	cmd.indexCount = indexCount;
	cmd.vertexOffset = vertexOffset;
	cmd.firstIndex = firstIndex;

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
	auto& cmd = addCmd<DrawIndirectCmd>(cb, cb);

	auto& buf = cb.dev->buffers.get(buffer);
	cmd.buffer = &buf;
	useHandle(cb, cmd, buf);

	cmd.indexed = false;
	cmd.offset = offset;
	cmd.drawCount = drawCount;
	cmd.stride = stride;

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
	auto& cmd = addCmd<DrawIndirectCmd>(cb, cb);

	auto& buf = cb.dev->buffers.get(buffer);
	cmd.buffer = &buf;
	useHandle(cb, cmd, buf);

	cmd.indexed = true;
	cmd.offset = offset;
	cmd.drawCount = drawCount;
	cmd.stride = stride;

	cb.dev->dispatch.CmdDrawIndexedIndirect(commandBuffer,
		buffer, offset, drawCount, stride);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndirectCount(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    buffer,
		VkDeviceSize                                offset,
		VkBuffer                                    countBuffer,
		VkDeviceSize                                countBufferOffset,
		uint32_t                                    maxDrawCount,
		uint32_t                                    stride) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<DrawIndirectCountCmd>(cb, cb);

	auto& buf = cb.dev->buffers.get(buffer);
	cmd.buffer = &buf;
	useHandle(cb, cmd, buf);

	auto& countBuf = cb.dev->buffers.get(countBuffer);
	cmd.countBuffer = &countBuf;
	useHandle(cb, cmd, countBuf);

	cmd.indexed = false;
	cmd.offset = offset;
	cmd.countBufferOffset = countBufferOffset;
	cmd.maxDrawCount = maxDrawCount;
	cmd.stride = stride;

	cb.dev->dispatch.CmdDrawIndirectCount(commandBuffer,
		buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirectCount(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    buffer,
		VkDeviceSize                                offset,
		VkBuffer                                    countBuffer,
		VkDeviceSize                                countBufferOffset,
		uint32_t                                    maxDrawCount,
		uint32_t                                    stride) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<DrawIndirectCountCmd>(cb, cb);

	auto& buf = cb.dev->buffers.get(buffer);
	cmd.buffer = &buf;
	useHandle(cb, cmd, buf);

	auto& countBuf = cb.dev->buffers.get(countBuffer);
	cmd.countBuffer = &countBuf;
	useHandle(cb, cmd, countBuf);

	cmd.indexed = true;
	cmd.offset = offset;
	cmd.countBufferOffset = countBufferOffset;
	cmd.maxDrawCount = maxDrawCount;
	cmd.stride = stride;

	cb.dev->dispatch.CmdDrawIndirectCount(commandBuffer,
		buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

VKAPI_ATTR void VKAPI_CALL CmdDispatch(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    groupCountX,
		uint32_t                                    groupCountY,
		uint32_t                                    groupCountZ) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<DispatchCmd>(cb, cb);

	cmd.groupsX = groupCountX;
	cmd.groupsY = groupCountY;
	cmd.groupsZ = groupCountZ;

	cb.dev->dispatch.CmdDispatch(commandBuffer,
		groupCountX, groupCountY, groupCountZ);
}

VKAPI_ATTR void VKAPI_CALL CmdDispatchIndirect(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    buffer,
		VkDeviceSize                                offset) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<DispatchIndirectCmd>(cb, cb);
	cmd.offset = offset;

	auto& buf = cb.dev->buffers.get(buffer);
	cmd.buffer = &buf;
	useHandle(cb, cmd, buf);

	cb.dev->dispatch.CmdDispatchIndirect(commandBuffer, buffer, offset);
}

VKAPI_ATTR void VKAPI_CALL CmdDispatchBase(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    baseGroupX,
		uint32_t                                    baseGroupY,
		uint32_t                                    baseGroupZ,
		uint32_t                                    groupCountX,
		uint32_t                                    groupCountY,
		uint32_t                                    groupCountZ) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<DispatchBaseCmd>(cb, cb);

	cmd.baseGroupX = baseGroupX;
	cmd.baseGroupY = baseGroupY;
	cmd.baseGroupZ = baseGroupZ;
	cmd.groupsX = groupCountX;
	cmd.groupsY = groupCountY;
	cmd.groupsZ = groupCountZ;

	cb.dev->dispatch.CmdDispatch(commandBuffer,
		groupCountX, groupCountY, groupCountZ);
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
	auto& cmd = addCmd<CopyImageCmd>(cb);

	auto& src = cb.dev->images.get(srcImage);
	auto& dst = cb.dev->images.get(dstImage);

	cmd.src = &src;
	cmd.srcLayout = srcImageLayout;
	cmd.dst = &dst;
	cmd.dstLayout = dstImageLayout;
	// TODO
	// cmd.copies = {pRegions, pRegions + regionCount};

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

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
	auto& cmd = addCmd<BlitImageCmd>(cb);

	auto& src = cb.dev->images.get(srcImage);
	auto& dst = cb.dev->images.get(dstImage);

	cmd.src = &src;
	cmd.srcLayout = srcImageLayout;
	cmd.dst = &dst;
	cmd.dstLayout = dstImageLayout;
	// TODO
	// cmd.blits = {pRegions, pRegions + regionCount};
	cmd.filter = filter;

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

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
	auto& cmd = addCmd<CopyBufferToImageCmd>(cb);

	auto& src = cb.dev->buffers.get(srcBuffer);
	auto& dst = cb.dev->images.get(dstImage);

	cmd.src = &src;
	cmd.dst = &dst;
	cmd.imgLayout = dstImageLayout;
	// TODO
	// cmd.copies = {pRegions, pRegions + regionCount};

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

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
	auto& cmd = addCmd<CopyImageToBufferCmd>(cb);

	auto& src = cb.dev->images.get(srcImage);
	auto& dst = cb.dev->buffers.get(dstBuffer);

	cmd.src = &src;
	cmd.dst = &dst;
	cmd.imgLayout = srcImageLayout;
	// TODO
	// cmd.copies = {pRegions, pRegions + regionCount};

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

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
	auto& cmd = addCmd<ClearColorImageCmd>(cb);

	auto& dst = cb.dev->images.get(image);
	cmd.dst = &dst;
	cmd.color = *pColor;
	cmd.imgLayout = imageLayout;
	cmd.ranges = {pRanges, pRanges + rangeCount};

	useHandle(cb, cmd, dst);

	cb.dev->dispatch.CmdClearColorImage(commandBuffer,
		image, imageLayout, pColor, rangeCount, pRanges);
}

VKAPI_ATTR void VKAPI_CALL CmdClearDepthStencilImage(
		VkCommandBuffer                             commandBuffer,
		VkImage                                     image,
		VkImageLayout                               imageLayout,
		const VkClearDepthStencilValue*             pDepthStencil,
		uint32_t                                    rangeCount,
		const VkImageSubresourceRange*              pRanges) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<ClearDepthStencilImageCmd>(cb);

	auto& dst = cb.dev->images.get(image);
	cmd.dst = &dst;
	cmd.imgLayout = imageLayout;
	cmd.value = *pDepthStencil;
	cmd.ranges = {pRanges, pRanges + rangeCount};

	useHandle(cb, cmd, dst);

	cb.dev->dispatch.CmdClearDepthStencilImage(commandBuffer, image,
		imageLayout, pDepthStencil, rangeCount, pRanges);
}

VKAPI_ATTR void VKAPI_CALL CmdClearAttachments(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    attachmentCount,
		const VkClearAttachment*                    pAttachments,
		uint32_t                                    rectCount,
		const VkClearRect*                          pRects) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<ClearAttachmentCmd>(cb);

	cmd.attachments = {pAttachments, pAttachments + attachmentCount};
	cmd.rects = {pRects, pRects + rectCount};

	// NOTE: add clears attachments handles to used handles for this cmd?
	// but they were already used in BeginRenderPass and are just implicitly
	// used here (as in many other render pass cmds). So we probably should
	// not do it.

	cb.dev->dispatch.CmdClearAttachments(commandBuffer, attachmentCount,
		pAttachments, rectCount, pRects);
}

VKAPI_ATTR void VKAPI_CALL CmdResolveImage(
		VkCommandBuffer                             commandBuffer,
		VkImage                                     srcImage,
		VkImageLayout                               srcImageLayout,
		VkImage                                     dstImage,
		VkImageLayout                               dstImageLayout,
		uint32_t                                    regionCount,
		const VkImageResolve*                       pRegions) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& src = cb.dev->images.get(srcImage);
	auto& dst = cb.dev->images.get(dstImage);

	auto& cmd = addCmd<ResolveImageCmd>(cb);
	cmd.src = &src;
	cmd.srcLayout = srcImageLayout;
	cmd.dst = &dst;
	cmd.dstLayout = dstImageLayout;
	cmd.regions = {pRegions, pRegions + regionCount};

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

	cb.dev->dispatch.CmdResolveImage(commandBuffer, srcImage, srcImageLayout,
		dstImage, dstImageLayout, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL CmdSetEvent(
		VkCommandBuffer                             commandBuffer,
		VkEvent                                     event,
		VkPipelineStageFlags                        stageMask) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<SetEventCmd>(cb);
	cmd.event = &cb.dev->events.get(event);
	cmd.stageMask = stageMask;

	useHandle(cb, cmd, *cmd.event);

	cb.dev->dispatch.CmdSetEvent(commandBuffer, event, stageMask);
}

VKAPI_ATTR void VKAPI_CALL CmdResetEvent(
		VkCommandBuffer                             commandBuffer,
		VkEvent                                     event,
		VkPipelineStageFlags                        stageMask) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<ResetEventCmd>(cb);
	cmd.event = &cb.dev->events.get(event);
	cmd.stageMask = stageMask;

	useHandle(cb, cmd, *cmd.event);

	cb.dev->dispatch.CmdSetEvent(commandBuffer, event, stageMask);
}

VKAPI_ATTR void VKAPI_CALL CmdExecuteCommands(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    commandBufferCount,
		const VkCommandBuffer*                      pCommandBuffers) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<ExecuteCommandsCmd>(cb);

	for(auto i = 0u; i < commandBufferCount; ++i) {
		auto& secondary = cb.dev->commandBuffers.get(pCommandBuffers[i]);
		cmd.secondaries.push_back(&secondary);
		useHandle(cb, cmd, secondary);

		for(auto& img : secondary.images) {
			if(img.second.layoutChanged) {
				useHandle(cb, cmd, *img.second.image, img.second.finalLayout);
			} else {
				useHandle(cb, cmd, *img.second.image);
			}
		}

		for(auto& buf : secondary.buffers) {
			useHandle(cb, cmd, *buf.second.buffer);
		}

		for(auto& handle : secondary.handles) {
			useHandle(cb, cmd, handle.first, *handle.second.handle);
		}
	}

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
	auto& cmd = addCmd<CopyBufferCmd>(cb);

	auto& srcBuf = cb.dev->buffers.get(srcBuffer);
	auto& dstBuf = cb.dev->buffers.get(dstBuffer);

	cmd.src = &srcBuf;
	cmd.dst = &dstBuf;
	cmd.regions = {pRegions, pRegions + regionCount};

	useHandle(cb, cmd, srcBuf);
	useHandle(cb, cmd, dstBuf);

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
	auto& cmd = addCmd<UpdateBufferCmd>(cb);

	auto& buf = cb.dev->buffers.get(dstBuffer);
	auto dataPtr = static_cast<const std::byte*>(pData);
	cmd.data = {dataPtr, dataPtr + dataSize};
	cmd.dst = &buf;
	cmd.offset = dstOffset;

	useHandle(cb, cmd, buf);

	cb.dev->dispatch.CmdUpdateBuffer(commandBuffer, dstBuffer, dstOffset, dataSize, pData);
}

VKAPI_ATTR void VKAPI_CALL CmdFillBuffer(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    dstBuffer,
		VkDeviceSize                                dstOffset,
		VkDeviceSize                                size,
		uint32_t                                    data) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<FillBufferCmd>(cb);

	auto& buf = cb.dev->buffers.get(dstBuffer);
	cmd.dst = &buf;
	cmd.offset = dstOffset;
	cmd.size = size;
	cmd.data = data;

	useHandle(cb, cmd, buf);

	cb.dev->dispatch.CmdFillBuffer(commandBuffer, dstBuffer, dstOffset, size, data);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginDebugUtilsLabelEXT(
		VkCommandBuffer                             commandBuffer,
		const VkDebugUtilsLabelEXT*                 pLabelInfo) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<BeginDebugUtilsLabelCmd, SectionType::begin>(cb, cb);

	auto* c = pLabelInfo->color;
	cmd.color = {c[0], c[1], c[2], c[3]};
	cmd.name = pLabelInfo->pLabelName;

	if(cb.dev->dispatch.CmdBeginDebugUtilsLabelEXT) {
		cb.dev->dispatch.CmdBeginDebugUtilsLabelEXT(commandBuffer, pLabelInfo);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdEndDebugUtilsLabelEXT(
		VkCommandBuffer                             commandBuffer) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<EndDebugUtilsLabelCmd, SectionType::end>(cb);
	(void) cmd;

	if(cb.dev->dispatch.CmdEndDebugUtilsLabelEXT) {
		cb.dev->dispatch.CmdEndDebugUtilsLabelEXT(commandBuffer);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(
		VkCommandBuffer                             commandBuffer,
		VkPipelineBindPoint                         pipelineBindPoint,
		VkPipeline                                  pipeline) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<BindPipelineCmd>(cb);
	cmd.bindPoint = pipelineBindPoint;

	if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
		cb.computeState.pipe = &cb.dev->computePipes.get(pipeline);
		cmd.pipe = cb.computeState.pipe;
	} else if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
		cb.graphicsState.pipe = &cb.dev->graphicsPipes.get(pipeline);
		cmd.pipe = cb.graphicsState.pipe;
	} else {
		dlg_error("unknown pipeline bind point");
	}

	useHandle(cb, cmd, *cmd.pipe);

	cb.dev->dispatch.CmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginQuery(
		VkCommandBuffer                             commandBuffer,
		VkQueryPool                                 queryPool,
		uint32_t                                    query,
		VkQueryControlFlags                         flags) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<BeginQueryCmd>(cb);
	cmd.pool = &cb.dev->queryPools.get(queryPool);
	cmd.query = query;
	cmd.flags = flags;

	useHandle(cb, cmd, *cmd.pool);

	cb.dev->dispatch.CmdBeginQuery(commandBuffer, queryPool, query, flags);
}

VKAPI_ATTR void VKAPI_CALL CmdEndQuery(
		VkCommandBuffer                             commandBuffer,
		VkQueryPool                                 queryPool,
		uint32_t                                    query) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<EndQueryCmd>(cb);
	cmd.pool = &cb.dev->queryPools.get(queryPool);
	cmd.query = query;

	useHandle(cb, cmd, *cmd.pool);

	cb.dev->dispatch.CmdEndQuery(commandBuffer, queryPool, query);
}

VKAPI_ATTR void VKAPI_CALL CmdResetQueryPool(
		VkCommandBuffer                             commandBuffer,
		VkQueryPool                                 queryPool,
		uint32_t                                    firstQuery,
		uint32_t                                    queryCount) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<ResetQueryPoolCmd>(cb);
	cmd.pool = &cb.dev->queryPools.get(queryPool);
	cmd.first = firstQuery;
	cmd.count = queryCount;

	useHandle(cb, cmd, *cmd.pool);

	cb.dev->dispatch.CmdResetQueryPool(commandBuffer, queryPool, firstQuery, queryCount);
}

VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp(
		VkCommandBuffer                             commandBuffer,
		VkPipelineStageFlagBits                     pipelineStage,
		VkQueryPool                                 queryPool,
		uint32_t                                    query) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<WriteTimestampCmd>(cb);
	cmd.pool = &cb.dev->queryPools.get(queryPool);
	cmd.stage = pipelineStage;
	cmd.query = query;

	useHandle(cb, cmd, *cmd.pool);

	cb.dev->dispatch.CmdWriteTimestamp(commandBuffer, pipelineStage, queryPool, query);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyQueryPoolResults(
		VkCommandBuffer                             commandBuffer,
		VkQueryPool                                 queryPool,
		uint32_t                                    firstQuery,
		uint32_t                                    queryCount,
		VkBuffer                                    dstBuffer,
		VkDeviceSize                                dstOffset,
		VkDeviceSize                                stride,
		VkQueryResultFlags                          flags) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<CopyQueryPoolResultsCmd>(cb);
	cmd.pool = &cb.dev->queryPools.get(queryPool);
	cmd.first = firstQuery;
	cmd.count = queryCount;
	cmd.dstBuffer = &cb.dev->buffers.get(dstBuffer);
	cmd.dstOffset = dstOffset;
	cmd.stride = stride;
	cmd.flags = flags;

	useHandle(cb, cmd, *cmd.pool);
	useHandle(cb, cmd, *cmd.dstBuffer);

	cb.dev->dispatch.CmdCopyQueryPoolResults(commandBuffer, queryPool,
		firstQuery, queryCount, dstBuffer, dstOffset, stride, flags);
}

VKAPI_ATTR void VKAPI_CALL CmdPushConstants(
		VkCommandBuffer                             commandBuffer,
		VkPipelineLayout                            pipeLayout,
		VkShaderStageFlags                          stageFlags,
		uint32_t                                    offset,
		uint32_t                                    size,
		const void*                                 pValues) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<PushConstantsCmd>(cb);

	// NOTE: See BindDescriptorSets for rationale on handling here.
	cmd.layout = cb.dev->pipeLayouts.getPtr(pipeLayout);

	cmd.stages = stageFlags;
	cmd.offset = offset;
	auto ptr = static_cast<const std::byte*>(pValues);
	cmd.values = {ptr, ptr + size};

	// TODO: not sure about this. The spec isn't clear about this.
	// But this seems to be what the validation layers do.
	// https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/57f6f2a387b37c442c4db6993eb064a1e750b30f/layers/state_tracker.cpp#L5868
	if(cb.pushConstants.layout &&
			pushConstantCompatible(*cmd.layout, *cb.pushConstants.layout)) {
		cb.pushConstants.layout = nullptr;
		cb.pushConstants.map.clear();
	}

	cb.pushConstants.layout = cmd.layout.get();
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

	cb.dev->dispatch.CmdPushConstants(commandBuffer, pipeLayout, stageFlags,
		offset, size, pValues);
}

VKAPI_ATTR void VKAPI_CALL CmdSetViewport(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    firstViewport,
		uint32_t                                    viewportCount,
		const VkViewport*                           pViewports) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<SetViewportCmd>(cb);
	cmd.first = firstViewport;
	cmd.viewports = {pViewports, pViewports + viewportCount};

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
	auto& cmd = addCmd<SetScissorCmd>(cb);
	cmd.first = firstScissor;
	cmd.scissors = {pScissors, pScissors + scissorCount};

	ensureSize(cb.graphicsState.dynamic.scissors, firstScissor + scissorCount);
	std::copy(pScissors, pScissors + scissorCount,
		cb.graphicsState.dynamic.scissors.begin() + firstScissor);

	cb.dev->dispatch.CmdSetScissor(commandBuffer, firstScissor, scissorCount, pScissors);
}

VKAPI_ATTR void VKAPI_CALL CmdSetLineWidth(
		VkCommandBuffer                             commandBuffer,
		float                                       lineWidth) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<SetLineWidthCmd>(cb);
	cmd.width = lineWidth;

	cb.graphicsState.dynamic.lineWidth = lineWidth;
	cb.dev->dispatch.CmdSetLineWidth(commandBuffer, lineWidth);
}

VKAPI_ATTR void VKAPI_CALL CmdSetDepthBias(
		VkCommandBuffer                             commandBuffer,
		float                                       depthBiasConstantFactor,
		float                                       depthBiasClamp,
		float                                       depthBiasSlopeFactor) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<SetDepthBiasCmd>(cb);
	cmd.state = {depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor};
	cb.graphicsState.dynamic.depthBias = cmd.state;

	cb.dev->dispatch.CmdSetDepthBias(commandBuffer,
		depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
}

VKAPI_ATTR void VKAPI_CALL CmdSetBlendConstants(
		VkCommandBuffer                             commandBuffer,
		const float                                 blendConstants[4]) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<SetBlendConstantsCmd>(cb);
	std::memcpy(cmd.values.data(), blendConstants, sizeof(cmd.values));
	std::memcpy(cb.graphicsState.dynamic.blendConstants.data(), blendConstants,
		sizeof(cb.graphicsState.dynamic.blendConstants));

	cb.dev->dispatch.CmdSetBlendConstants(commandBuffer, blendConstants);
}

VKAPI_ATTR void VKAPI_CALL CmdSetDepthBounds(
		VkCommandBuffer                             commandBuffer,
		float                                       minDepthBounds,
		float                                       maxDepthBounds) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<SetDepthBoundsCmd>(cb);
	cmd.min = minDepthBounds;
	cmd.max = maxDepthBounds;
	cb.graphicsState.dynamic.depthBoundsMin = minDepthBounds;
	cb.graphicsState.dynamic.depthBoundsMax = maxDepthBounds;

	cb.dev->dispatch.CmdSetDepthBounds(commandBuffer, minDepthBounds, maxDepthBounds);
}

VKAPI_ATTR void VKAPI_CALL CmdSetStencilCompareMask(
		VkCommandBuffer                             commandBuffer,
		VkStencilFaceFlags                          faceMask,
		uint32_t                                    compareMask) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<SetStencilCompareMaskCmd>(cb);
	cmd.faceMask = faceMask;
	cmd.value = compareMask;
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
	auto& cmd = addCmd<SetStencilWriteMaskCmd>(cb);
	cmd.faceMask = faceMask;
	cmd.value = writeMask;
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
	auto& cmd = addCmd<SetStencilReferenceCmd>(cb);
	cmd.faceMask = faceMask;
	cmd.value = reference;
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
		if(!compatibleForSetN(*descriptorSets[i].layout, layout, u32(lastSet))) {
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
