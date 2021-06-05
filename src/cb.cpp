#include <cb.hpp>
#include <data.hpp>
#include <ds.hpp>
#include <queue.hpp>
#include <handles.hpp>
#include <rt.hpp>
#include <threadContext.hpp>
#include <command/commands.hpp>
#include <gui/commandHook.hpp>
#include <util/util.hpp>
#include <util/ext.hpp>
#include <util/profiling.hpp>

namespace vil {

// CommandBuffer
CommandBuffer::CommandBuffer(CommandPool& xpool, VkCommandBuffer xhandle) :
		pool_(&xpool), handle_(xhandle) {
}

CommandBuffer::~CommandBuffer() {
	if(!dev) {
		return;
	}

	dlg_assert(pool_);
	dlg_assert(handle_);

	// Wait for completion, free all data and allocated stuff,
	// unregister from everything
	{
		std::lock_guard lock(dev->mutex);
		clearPendingLocked();

		if(record_) {
			dlg_assert(record_->cb == this);
			record_->cb = nullptr;
			record_->hookRecords.clear();
		}
	}

	// CommandBuffer is dispatchable handle, we need to remove this association
	if(!HandleDesc<VkCommandBuffer>::wrap) {
		eraseData(handle_);
	}

	// Remove ourselves from the pool we come from.
	// A command pool can't be destroyed before its command buffers (it
	// implicitly frees them).
	auto it = find(pool_->cbs, this);
	dlg_assert(it != pool_->cbs.end());
	pool_->cbs.erase(it);
}

void CommandBuffer::clearPendingLocked() {
	ZoneScoped;

	// checkLocked will automtically remove it from this cb
	while(!this->pending.empty()) {
		auto res = checkLocked(*this->pending.front()->parent);
		dlg_assert(res);
	}

	// NOTE: may be unexpected to do this here.
	// But in all places where we can be sure all submissions were
	// finished, we have to make sure to invalidate primary cbs.
	this->invalidateCbsLocked();
}

void CommandBuffer::doReset(bool startRecord) {
	ZoneScoped;

	// Make sure to never destroy a CommandBufferRecord inside the
	// device lock.
	IntrusivePtr<CommandRecord> keepAliveRecord;

	{
		std::lock_guard lock(dev->mutex);
		clearPendingLocked();

		if(record_) {
			record_->cb = nullptr;
			record_->hookRecords.clear();
			keepAliveRecord = std::move(record_);
		}

		// We have to lock our own mutex since other threads might read
		// our data at the same time.
		if(startRecord) {
			++recordCount_;

			record_ = IntrusivePtr<CommandRecord>(new CommandRecord(*this));
			state_ = CommandBuffer::State::recording;
		} else {
			state_ = CommandBuffer::State::initial;
		}
	}
}

void CommandBuffer::doEnd() {
	ZoneScoped;
	dlg_assert(record_);

	// debug utils labels can be unterminated, see docs/debug-utils-label-nesting.md
	while(section_) {
		auto lblCmd = dynamic_cast<const BeginDebugUtilsLabelCmd*>(section_->cmd);
		dlg_assert(lblCmd);
		dlg_assert(!section_->pop);
		record_->pushLables.push_back(lblCmd->name);
		section_ = section_->parent;
	}

	dlg_assert(!section_);

	graphicsState_ = {};
	computeState_ = {};
	rayTracingState_ = {};
	pushConstants_ = {};

	section_ = nullptr;
	lastCommand_ = nullptr;
	ignoreEndDebugLabels_ = 0u;

	// parse commands into description
	// record_->desc = CommandBufferDesc::get(record_->commands);
	{
		ZoneScopedN("getAnnotate");
		record_->desc = CommandBufferDesc::getAnnotate(record_->commands);
	}

	// Make sure to never call CommandBufferRecord destructor inside lock.
	// Don't just call reset() here or move lastRecord_ so that always have a valid
	// lastRecord_ as state (some other thread could query it before we lock)
	auto keepAliveRecord = lastRecord_;

	// Critical section
	{
		std::lock_guard lock(dev->mutex);
		ZoneScopedN("addToHandles");

		dlg_assert(state_ == State::recording);
		dlg_assert(!record_->finished);

		state_ = State::executable;

		for(auto& image : record_->images) {
			image.second.image->refRecords.insert(record_.get());
		}

		for(auto& handle : record_->handles) {
			handle.second.handle->refRecords.insert(record_.get());
		}

		record_->finished = true;
		lastRecord_ = record_; // could use std::move, record_ is only really used while recording
	}
}

void CommandBuffer::endSection(Command* cmd) {
	if(!section_) {
		// Debug utils commands can span multiple command buffers, they
		// are only queue-local.
		// See docs/debug-utils-label-nesting.md
		dlg_assert(dynamic_cast<EndDebugUtilsLabelCmd*>(cmd));
		++record_->numPopLabels;
		return;
	}

	lastCommand_ = section_->cmd;
	dlg_assert(!section_->pop); // we shouldn't be able to land here

	// reset it for future use
	section_->cmd = nullptr;
	section_->pop = false;

	// Don't unset section_->next, we can re-use the allocation
	// later on. We unset 'cmd' above to signal its unused (as debug check)
	section_ = section_->parent;

	// We pop the label sections here that were previously ended by
	// the application but not in the same nesting level they were created.
	while(section_ && section_->pop) {
		dlg_assert(dynamic_cast<BeginDebugUtilsLabelCmd*>(section_->cmd));
		lastCommand_ = section_->cmd;

		// reset it for future use
		section_->cmd = nullptr;
		section_->pop = false;

		section_ = section_->parent;
	}
}

void CommandBuffer::popLabelSections() {
	// See docs/debug-utils-label-nesting.md
	while(auto* next = dynamic_cast<BeginDebugUtilsLabelCmd*>(section_->cmd)) {
		dlg_trace("Problematic debug utils label nesting detected "
			"(Begin without end in scope): {}", next->name);
		section_ = section_->parent;
		++ignoreEndDebugLabels_;
	}
}

void CommandBuffer::beginSection(SectionCommand& cmd) {
	if(section_) {
		// re-use a previously allocated section that isn't in use anymore
		if(section_->next) {
			dlg_assert(!section_->next->cmd);
			section_ = section_->next;
			section_->cmd = nullptr;
			section_->pop = false;
		} else {
			auto nextSection = &vil::allocate<Section>(*this);
			nextSection->parent = section_;
			section_->next = nextSection;
			section_ = nextSection;
		}
	} else {
		section_ = &vil::allocate<Section>(*this);
	}

	section_->cmd = &cmd;
	lastCommand_ = nullptr;
}

void CommandBuffer::addCmd(Command& cmd) {
	if(lastCommand_) {
		dlg_assert(record_->commands);
		lastCommand_->next = &cmd;
	} else if(section_) {
		dlg_assert(record_->commands);
		dlg_assert(section_->cmd);
		dlg_assert(!section_->cmd->children_);
		section_->cmd->children_ = &cmd;
	} else if(!record_->commands) {
		record_->commands = &cmd;
	}

	lastCommand_ = &cmd;
}

void CommandBuffer::invalidateLocked() {
	// It's impossible that the command buffer is invalidated while
	// recording. While it's theoretically possible per vulkan spec,
	// it's practically the same as just never finishing the recording phase
	// and so we treat it like that. Applications that depend on this
	// behavior are kinda weird/broken (since that means they
	// call BeginCommandBuffer, use a handle, destroy/change the handle
	// and never call EndCommandBuffer). But in our case, this function
	// can never be called in recording state since we haven't added ourself
	// to handles yet (this assumption is important below).
	dlg_assert(state_ == State::executable);
	dlg_assert(record_);
	dlg_assert(record_.get() == lastRecord_.get());

	clearPendingLocked();

	// Free the hook data (as soon as possible), it's no longer
	// needed as this record will never be submitted again.
	record_->hookRecords.clear();
	record_->cb = nullptr;

	// We can do this safely here, without ever causing the destructor
	// to be called (which would be a problem since this function is
	// called when device mutex is locked) since lastRecord_ must be
	// the same as record_ (see assert above).
	record_ = nullptr;

	this->state_ = State::invalid;
}

CommandPool::~CommandPool() {
	if(!dev) {
		return;
	}

	// When a CommandPool is destroyed, all command buffers created from
	// it are automatically freed.
	// NOTE: we don't need a lock here:
	// While the command pool is being destroyed, no command buffers from it
	// can be created or destroyed in another thread, that would always be a
	// race. So accessing this vector is safe.
	// (Just adding a lock here would furthermore result in deadlocks due
	// to the mutexes locked inside the loop, don't do it!)
	// We don't use a for loop since the command buffers remove themselves
	// on destruction

	while(!cbs.empty()) {
		if(HandleDesc<VkCommandBuffer>::wrap) {
			// TODO: ugh, this is terrible, should find a cleaner solution
			auto* cbp = reinterpret_cast<std::byte*>(cbs[0]);
			cbp -= offsetof(WrappedHandle<CommandBuffer>, obj_);
			auto h = u64ToHandle<VkCommandBuffer>(reinterpret_cast<std::uintptr_t>(cbp));
			dev->commandBuffers.mustErase(h);
		} else {
			auto* cb = cbs[0];
			dev->commandBuffers.mustErase(cb->handle());
		}
	}
}

// recording
enum class SectionType {
	none,
	begin,
	next,
	end,
};

template<typename T, SectionType ST = SectionType::none, typename... Args>
T& addCmd(CommandBuffer& cb, Args&&... args) {
	static_assert(std::is_base_of_v<Command, T>);

	// We require all commands to be trivially destructible as we
	// never destroy them. They are only meant to store data, not hold
	// ownership of anything.
	static_assert(std::is_trivially_destructible_v<T>);

	dlg_assert(cb.state() == CommandBuffer::State::recording);
	dlg_assert(cb.record());

	auto& cmd = vil::allocate<T>(cb, std::forward<Args>(args)...);

	if constexpr(ST == SectionType::end || ST == SectionType::next) {
		cb.endSection(&cmd);
	}

	cb.addCmd(cmd);

	if constexpr(ST == SectionType::begin || ST == SectionType::next) {
		static_assert(std::is_convertible_v<T*, SectionCommand*>);
		cb.beginSection(cmd);
	}

	return cmd;
}

// api
// command pool
VKAPI_ATTR VkResult VKAPI_CALL CreateCommandPool(
		VkDevice                                    device,
		const VkCommandPoolCreateInfo*              pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkCommandPool*                              pCommandPool) {
	auto& dev = getDevice(device);

	auto res = dev.dispatch.CreateCommandPool(dev.handle, pCreateInfo, pAllocator, pCommandPool);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto cpPtr = std::make_unique<CommandPool>();
	auto& cp = *cpPtr;
	cp.objectType = VK_OBJECT_TYPE_COMMAND_POOL;
	cp.dev = &dev;
	cp.handle = *pCommandPool;
	cp.queueFamily = pCreateInfo->queueFamilyIndex;

	*pCommandPool = castDispatch<VkCommandPool>(cp);
	dev.commandPools.mustEmplace(*pCommandPool, std::move(cpPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(
		VkDevice                                    device,
		VkCommandPool                               commandPool,
		const VkAllocationCallbacks*                pAllocator) {
	if(!commandPool) {
		return;
	}

	auto& dev = getDevice(device);
	auto handle = dev.commandPools.mustMove(commandPool)->handle;
	dev.dispatch.DestroyCommandPool(dev.handle, handle, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL ResetCommandPool(
		VkDevice                                    device,
		VkCommandPool                               commandPool,
		VkCommandPoolResetFlags                     flags) {
	auto& cp = get(device, commandPool);
	for(auto* cb : cp.cbs) {
		cb->doReset(false);
	}

	return cp.dev->dispatch.ResetCommandPool(cp.dev->handle, cp.handle, flags);
}

VKAPI_ATTR void VKAPI_CALL TrimCommandPool(
		VkDevice                                    device,
		VkCommandPool                               commandPool,
		VkCommandPoolTrimFlags                      flags) {
	auto& pool = get(device, commandPool);
	pool.dev->dispatch.TrimCommandPool(pool.dev->handle, pool.handle, flags);
}

// command buffer
VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(
		VkDevice                                    device,
		const VkCommandBufferAllocateInfo*          pAllocateInfo,
		VkCommandBuffer*                            pCommandBuffers) {
	auto& pool = get(device, pAllocateInfo->commandPool);
	auto& dev = *pool.dev;

	auto nci = *pAllocateInfo;
	nci.commandPool = pool.handle;

	auto res = dev.dispatch.AllocateCommandBuffers(dev.handle, &nci, pCommandBuffers);
	if(res != VK_SUCCESS) {
		return res;
	}

	for(auto i = 0u; i < pAllocateInfo->commandBufferCount; ++i) {
		auto handle = pCommandBuffers[i];

		auto cbPtr = UniqueWrappedPtr<CommandBuffer>(new WrappedHandle<CommandBuffer>(pool, handle));
		auto& cb = *cbPtr;

		cb.dev = &dev;
		cb.objectType = VK_OBJECT_TYPE_COMMAND_BUFFER;
		cb.pool().cbs.push_back(&cb);

		// command buffers are dispatchable, add global data entry
		if(!HandleDesc<VkCommandBuffer>::wrap) {
			vil::insertData(cb.handle(), &cb);
		} else {
			dev.setDeviceLoaderData(dev.handle, pCommandBuffers[i]);
			pCommandBuffers[i] = castDispatch<VkCommandBuffer>(dev, *cbPtr.wrapped());
		}

		dev.commandBuffers.mustEmplace(pCommandBuffers[i], std::move(cbPtr));
	}

	return res;
}

VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers(
		VkDevice                                    device,
		VkCommandPool                               commandPool,
		uint32_t                                    commandBufferCount,
		const VkCommandBuffer*                      pCommandBuffers) {
	auto& pool = get(device, commandPool);
	auto& dev = *pool.dev;

	ThreadMemScope memScope;
	auto handles = memScope.alloc<VkCommandBuffer>(commandBufferCount);

	for(auto i = 0u; i < commandBufferCount; ++i) {
		handles[i] = dev.commandBuffers.mustMove(pCommandBuffers[i])->handle();
	}

	dev.dispatch.FreeCommandBuffers(dev.handle, pool.handle,
		commandBufferCount, handles.data());
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(
		VkCommandBuffer                             commandBuffer,
		const VkCommandBufferBeginInfo*             pBeginInfo) {
	auto& cb = getCommandBuffer(commandBuffer);
	cb.doReset(true);
	cb.record()->usageFlags = pBeginInfo->flags;

	VkCommandBufferInheritanceInfo inherit;
	auto beginInfo = *pBeginInfo;

	if(pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) {
		dlg_assert(pBeginInfo->pInheritanceInfo);
		dlg_assert(pBeginInfo->pInheritanceInfo->renderPass);

		auto& rp = cb.dev->renderPasses.get(pBeginInfo->pInheritanceInfo->renderPass);
		cb.graphicsState().rpi.rp = &rp;
		cb.graphicsState().rpi.subpass = pBeginInfo->pInheritanceInfo->subpass;

		inherit = *pBeginInfo->pInheritanceInfo;
		inherit.renderPass = rp.handle;

		if(pBeginInfo->pInheritanceInfo->framebuffer) {
			auto& fb = cb.dev->framebuffers.get(pBeginInfo->pInheritanceInfo->framebuffer);

			// not sure if imageless framebuffers are allowed here.
			dlg_assert(!fb.imageless);
			if(!fb.imageless) {
				dlg_assert(rp.desc->attachments.size() == fb.attachments.size());
				cb.graphicsState().rpi.attachments = allocSpan<ImageView*>(cb, fb.attachments.size());

				for(auto i = 0u; i < fb.attachments.size(); ++i) {
					auto& attachment = fb.attachments[i];
					dlg_assert(attachment && attachment->img);

					// TODO: same issue as below, would need to allow usedHandles
					// without commands.
					// useHandle(cb, cmd, *attachment, false);

					cb.graphicsState().rpi.attachments[i] = attachment;
				}

			}

			inherit.framebuffer = fb.handle;
		}

		beginInfo.pInheritanceInfo = &inherit;

		// TODO: use handles here? would have to allow using handles without command
		// Pretty sure we'd have to do it for correctness.
		// useHandle(cb, cmd, *cmd.fb);
		// useHandle(cb, cmd, *cmd.rp);
	}

	return cb.dev->dispatch.BeginCommandBuffer(cb.handle(), &beginInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL EndCommandBuffer(
		VkCommandBuffer                             commandBuffer) {
	auto& cb = getCommandBuffer(commandBuffer);
	cb.doEnd();
	return cb.dev->dispatch.EndCommandBuffer(cb.handle());
}

VKAPI_ATTR VkResult VKAPI_CALL ResetCommandBuffer(
		VkCommandBuffer                             commandBuffer,
		VkCommandBufferResetFlags                   flags) {
	auto& cb = getCommandBuffer(commandBuffer);
	cb.doReset(false);
	return cb.dev->dispatch.ResetCommandBuffer(cb.handle(), flags);
}

// == command buffer recording ==
// util
void useHandle(CommandRecord& rec, Command& cmd, u64 h64, DeviceHandle& handle) {
	auto it = rec.handles.emplace(h64, UsedHandle{handle, rec}).first;
	it->second.commands.push_back(&cmd);
}

template<typename T>
void useHandle(CommandRecord& rec, Command& cmd, T& handle) {
	auto h64 = handleToU64(vil::handle(handle));
	useHandle(rec, cmd, h64, handle);
}

UsedImage& useHandle(CommandRecord& rec, Command& cmd, Image& image) {
	auto it = rec.images.emplace(image.handle, UsedImage{image, rec}).first;
	it->second.commands.push_back(&cmd);

	dlg_assert(it->second.image);
	it->second.commands.push_back(&cmd);

	// NOTE: add swapchain in case it's a swapchain image?
	// shouldn't be needed I guess.
	// NOTE: can currently fail for sparse bindings i guess
	dlg_assert(image.memory || image.swapchain);
	if(image.memory) {
		useHandle(rec, cmd, *image.memory);
	}

	return it->second;
}

void useHandle(CommandRecord& rec, Command& cmd, ImageView& view, bool useImg = true) {
	auto h64 = handleToU64(view.handle);
	useHandle(rec, cmd, h64, view);

	dlg_assert(view.img);
	if(useImg && view.img) {
		useHandle(rec, cmd, *view.img);
	}
}

void useHandle(CommandRecord& rec, Command& cmd, Buffer& buf) {
	auto h64 = handleToU64(buf.handle);
	useHandle(rec, cmd, h64, buf);

	// NOTE: can currently fail for sparse bindings i guess
	dlg_assert(buf.memory);
	if(buf.memory) {
		useHandle(rec, cmd, *buf.memory);
	}
}

void useHandle(CommandRecord& rec, Command& cmd, BufferView& view) {
	auto h64 = handleToU64(view.handle);
	useHandle(rec, cmd, h64, view);

	dlg_assert(view.buffer);
	if(view.buffer) {
		useHandle(rec, cmd, *view.buffer);
	}
}

void useHandle(CommandRecord& rec, Command& cmd, Image& image, VkImageLayout newLayout) {
	auto& img = useHandle(rec, cmd, image);
	img.layoutChanged = true;
	img.finalLayout = newLayout;
}

template<typename... Args>
void useHandle(CommandBuffer& cb, Args&&... args) {
	dlg_assert(cb.state() == CommandBuffer::State::recording);
	dlg_assert(cb.record());
	useHandle(*cb.record(), std::forward<Args>(args)...);
}

// commands
void cmdBarrier(
		CommandBuffer& 								cb,
		BarrierCmdBase& 							cmd,
		VkPipelineStageFlags                        srcStageMask,
		VkPipelineStageFlags                        dstStageMask) {
	ExtZoneScoped;

	cmd.srcStageMask = srcStageMask;
	cmd.dstStageMask = dstStageMask;

	cmd.images = allocSpan<Image*>(cb, cmd.imgBarriers.size());
	for(auto i = 0u; i < cmd.imgBarriers.size(); ++i) {
		auto& imgb = cmd.imgBarriers[i];
		copyChainInPlace(cb, imgb.pNext);

		auto& img = cb.dev->images.get(imgb.image);
		cmd.images[i] = &img;
		useHandle(cb, cmd, img, imgb.newLayout);

		// When the image was put into concurrent sharing mode by us,
		// we have to make sure this does not actually define a queue
		// family transition since those are not allowed for concurrent images
		if(img.concurrentHooked) {
			imgb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imgb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		}

		imgb.image = img.handle;
	}

	cmd.buffers = allocSpan<Buffer*>(cb, cmd.bufBarriers.size());
	for(auto i = 0u; i < cmd.bufBarriers.size(); ++i) {
		auto& bufb = cmd.bufBarriers[i];
		copyChainInPlace(cb, bufb.pNext);

		auto& buf = cb.dev->buffers.get(bufb.buffer);
		cmd.buffers[i] = &buf;
		useHandle(cb, cmd, buf);

		if(buf.concurrentHooked) {
			bufb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			bufb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		}

		bufb.buffer = buf.handle;
	}

	for(auto& mem : cmd.memBarriers) {
		copyChainInPlace(cb, mem.pNext);
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
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<WaitEventsCmd>(cb);

	cmd.memBarriers = copySpan(cb, pMemoryBarriers, memoryBarrierCount);
	cmd.imgBarriers = copySpan(cb, pImageMemoryBarriers, imageMemoryBarrierCount);
	cmd.bufBarriers = copySpan(cb, pBufferMemoryBarriers, bufferMemoryBarrierCount);

	cmdBarrier(cb, cmd, srcStageMask, dstStageMask);

	cmd.events = allocSpan<Event*>(cb, eventCount);
	for(auto i = 0u; i < eventCount; ++i) {
		auto& event = cb.dev->events.get(pEvents[i]);
		cmd.events[i] = &event;
		useHandle(cb, cmd, event);
	}

	cb.dev->dispatch.CmdWaitEvents(cb.handle(), eventCount, pEvents,
		srcStageMask, dstStageMask,
		u32(cmd.memBarriers.size()), cmd.memBarriers.data(),
		u32(cmd.bufBarriers.size()), cmd.bufBarriers.data(),
		u32(cmd.imgBarriers.size()), cmd.imgBarriers.data());
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
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<BarrierCmd>(cb);
	cmd.dependencyFlags = dependencyFlags;

	cmd.memBarriers = copySpan(cb, pMemoryBarriers, memoryBarrierCount);
	cmd.imgBarriers = copySpan(cb, pImageMemoryBarriers, imageMemoryBarrierCount);
	cmd.bufBarriers = copySpan(cb, pBufferMemoryBarriers, bufferMemoryBarrierCount);

	cmdBarrier(cb, cmd, srcStageMask, dstStageMask);

	cb.dev->dispatch.CmdPipelineBarrier(cb.handle(),
		srcStageMask, dstStageMask, dependencyFlags,
		u32(cmd.memBarriers.size()), cmd.memBarriers.data(),
		u32(cmd.bufBarriers.size()), cmd.bufBarriers.data(),
		u32(cmd.imgBarriers.size()), cmd.imgBarriers.data());
}

void cmdBeginRenderPass(CommandBuffer& cb,
		VkRenderPassBeginInfo& rpBeginInfo,
		const VkSubpassBeginInfo& subpassBeginInfo) {
	auto& cmd = addCmd<BeginRenderPassCmd, SectionType::begin>(cb);

	cmd.clearValues = copySpan(cb, rpBeginInfo.pClearValues, rpBeginInfo.clearValueCount);
	cmd.info = rpBeginInfo;
	cmd.info.pClearValues = cmd.clearValues.data();
	copyChainInPlace(cb, cmd.info.pNext);

	cmd.fb = cb.dev->framebuffers.find(rpBeginInfo.framebuffer);
	cmd.rp = cb.dev->renderPasses.find(rpBeginInfo.renderPass);

	dlg_assert(!cb.graphicsState().rpi.rp);
	cb.graphicsState().rpi.rp = cmd.rp;
	cb.graphicsState().rpi.subpass = 0u;

	cmd.subpassBeginInfo = subpassBeginInfo;
	copyChainInPlace(cb, cmd.subpassBeginInfo.pNext);

	dlg_assert(cmd.fb);
	dlg_assert(cmd.rp);

	useHandle(cb, cmd, *cmd.fb);
	useHandle(cb, cmd, *cmd.rp);

	if(cmd.fb->imageless) {
		constexpr auto sType = VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO;
		auto* attInfo = findChainInfo<VkRenderPassAttachmentBeginInfo, sType>(rpBeginInfo);
		dlg_assert(attInfo);

		dlg_assert(cmd.rp->desc->attachments.size() == attInfo->attachmentCount);
		cb.graphicsState().rpi.attachments = allocSpan<ImageView*>(cb, attInfo->attachmentCount);

		for(auto i = 0u; i < attInfo->attachmentCount; ++i) {
			auto& attachment = get(*cb.dev, attInfo->pAttachments[i]);
			dlg_assert(attachment.img);

			useHandle(cb, cmd, attachment, false);
			useHandle(cb, cmd, nonNull(attachment.img),
				cmd.rp->desc->attachments[i].finalLayout);

			cb.graphicsState().rpi.attachments[i] = &attachment;
		}
	} else {
		dlg_assert(cmd.rp->desc->attachments.size() == cmd.fb->attachments.size());
		cb.graphicsState().rpi.attachments = allocSpan<ImageView*>(cb, cmd.fb->attachments.size());

		for(auto i = 0u; i < cmd.fb->attachments.size(); ++i) {
			auto& attachment = cmd.fb->attachments[i];
			dlg_assert(attachment && attachment->img);

			useHandle(cb, cmd, *attachment, false);
			useHandle(cb, cmd, *attachment->img,
				cmd.rp->desc->attachments[i].finalLayout);

			cb.graphicsState().rpi.attachments[i] = attachment;
		}
	}

	auto& subpassCmd = addCmd<FirstSubpassCmd, SectionType::begin>(cb);
	(void) subpassCmd;

	rpBeginInfo.framebuffer = cmd.fb->handle;
	rpBeginInfo.renderPass = cmd.rp->handle;
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(
		VkCommandBuffer                             commandBuffer,
		const VkRenderPassBeginInfo*                pRenderPassBegin,
		VkSubpassContents                           contents) {
	auto& cb = getCommandBuffer(commandBuffer);

	VkSubpassBeginInfo subpassBeginInfo = {};
	subpassBeginInfo.contents = contents;
	subpassBeginInfo.sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO;

	auto beginInfo = *pRenderPassBegin;
	cmdBeginRenderPass(cb, beginInfo, subpassBeginInfo);

	cb.dev->dispatch.CmdBeginRenderPass(cb.handle(), &beginInfo, contents);
}

VKAPI_ATTR void VKAPI_CALL CmdNextSubpass(
		VkCommandBuffer                             commandBuffer,
		VkSubpassContents                           contents) {
	auto& cb = getCommandBuffer(commandBuffer);

	cb.popLabelSections();
	dlg_assert(cb.section() && dynamic_cast<SubpassCmd*>(cb.section()->cmd));

	auto& cmd = addCmd<NextSubpassCmd, SectionType::next>(cb);
	cmd.beginInfo = {};
	cmd.beginInfo.sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO;
	cmd.beginInfo.contents = contents;

	dlg_assert(cb.graphicsState().rpi.rp);
	++cb.graphicsState().rpi.subpass;

	cmd.subpassID = cb.graphicsState().rpi.subpass;

	cb.dev->dispatch.CmdNextSubpass(cb.handle(), contents);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(
		VkCommandBuffer                             commandBuffer) {
	auto& cb = getCommandBuffer(commandBuffer);

	cb.popLabelSections();
	dlg_assert(cb.section() && dynamic_cast<SubpassCmd*>(cb.section()->cmd));

	cb.endSection(nullptr); // pop subpass section
	dlg_assert(cb.section() && dynamic_cast<BeginRenderPassCmd*>(cb.section()->cmd));

	auto& cmd = addCmd<EndRenderPassCmd, SectionType::end>(cb);
	cmd.endInfo = {};
	cmd.endInfo.sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO;

	dlg_assert(cb.graphicsState().rpi.rp);
	cb.graphicsState().rpi = {};

	cb.dev->dispatch.CmdEndRenderPass(cb.handle());
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass2(
		VkCommandBuffer                             commandBuffer,
		const VkRenderPassBeginInfo*                pRenderPassBegin,
		const VkSubpassBeginInfo*                   pSubpassBeginInfo) {
	auto& cb = getCommandBuffer(commandBuffer);

	auto beginInfo = *pRenderPassBegin;
	cmdBeginRenderPass(cb, beginInfo, *pSubpassBeginInfo);

	cb.dev->dispatch.CmdBeginRenderPass2(cb.handle(), &beginInfo, pSubpassBeginInfo);
}

VKAPI_ATTR void VKAPI_CALL CmdNextSubpass2(
		VkCommandBuffer                             commandBuffer,
		const VkSubpassBeginInfo*                   pSubpassBeginInfo,
		const VkSubpassEndInfo*                     pSubpassEndInfo) {
	auto& cb = getCommandBuffer(commandBuffer);

	cb.popLabelSections();
	dlg_assert(cb.section() && dynamic_cast<SubpassCmd*>(cb.section()->cmd));

	auto& cmd = addCmd<NextSubpassCmd, SectionType::next>(cb);
	cmd.beginInfo = *pSubpassBeginInfo;
	copyChainInPlace(cb, cmd.beginInfo.pNext);

	dlg_assert(cb.graphicsState().rpi.rp);
	++cb.graphicsState().rpi.subpass;

	cmd.subpassID = cb.graphicsState().rpi.subpass;

	cb.dev->dispatch.CmdNextSubpass2(cb.handle(), pSubpassBeginInfo, pSubpassEndInfo);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass2(
		VkCommandBuffer                             commandBuffer,
		const VkSubpassEndInfo*                     pSubpassEndInfo) {
	auto& cb = getCommandBuffer(commandBuffer);

	cb.popLabelSections();
	dlg_assert(cb.section() && dynamic_cast<SubpassCmd*>(cb.section()->cmd));

	cb.endSection(nullptr); // pop subpass section
	dlg_assert(cb.section() && dynamic_cast<BeginRenderPassCmd*>(cb.section()->cmd));

	auto& cmd = addCmd<EndRenderPassCmd, SectionType::end>(cb);
	cmd.endInfo = *pSubpassEndInfo;
	copyChainInPlace(cb, cmd.endInfo.pNext);

	dlg_assert(cb.graphicsState().rpi.rp);
	cb.graphicsState().rpi = {};

	cb.dev->dispatch.CmdEndRenderPass2(cb.handle(), pSubpassEndInfo);
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
	ExtZoneScoped;

	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<BindDescriptorSetCmd>(cb);

	cmd.firstSet = firstSet;
	cmd.pipeBindPoint = pipelineBindPoint;
	cmd.dynamicOffsets = copySpan(cb, pDynamicOffsets, dynamicOffsetCount);

	// NOTE: the pipeline layout is intetionally not added to used handles
	// since the application not destroying it does not move the command
	// buffer into invalid state (and vulkan requires that it's kept
	// alive while recording).
	// Since we might need it later on, we acquire shared ownership.
	// Also like this in CmdPushConstants
	auto pipeLayoutPtr = getPtr(*cb.dev, layout);
	cmd.pipeLayout = pipeLayoutPtr.get();
	cb.record()->pipeLayouts.emplace_back(std::move(pipeLayoutPtr));

	cmd.sets = allocSpan<DescriptorSet*>(cb, descriptorSetCount);
	auto setHandles = LocalVector<VkDescriptorSet>(descriptorSetCount);
	for(auto i = 0u; i < descriptorSetCount; ++i) {
		auto& ds = get(*cb.dev, pDescriptorSets[i]);

		cmd.sets[i] = &ds;
		setHandles[i] = ds.handle;

		useHandle(cb, cmd, ds);
	}

	// TODO: not sure about this. The spec isn't clear about this.
	// But this seems to be what the validation layers do.
	// https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/57f6f2a387b37c442c4db6993eb064a1e750b30f/layers/state_tracker.cpp#L5868
	// if(cb.pushConstants.layout &&
	// 		!pushConstantCompatible(*cmd.pipeLayout, *cb.pushConstants.layout)) {
	// 	cb.pushConstants.layout = nullptr;
	// 	cb.pushConstants.map.clear();
	// }

	// TODO(perf): we probably don't want to track all this invalidation stuff
	// not meaningful in UI, we should just assume it's valid.
	// Then we also don't need to track the pipeline layout

	// update bound state
	if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
		cb.computeState().bind(cb, *cmd.pipeLayout, firstSet, cmd.sets,
			{pDynamicOffsets, pDynamicOffsets + dynamicOffsetCount});
	} else if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
		cb.graphicsState().bind(cb, *cmd.pipeLayout, firstSet, cmd.sets,
			{pDynamicOffsets, pDynamicOffsets + dynamicOffsetCount});
	} else if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) {
		cb.rayTracingState().bind(cb, *cmd.pipeLayout, firstSet, cmd.sets,
			{pDynamicOffsets, pDynamicOffsets + dynamicOffsetCount});
	} else {
		dlg_error("Unknown pipeline bind point");
	}

	{
		ExtZoneScopedN("dispatch");
		cb.dev->dispatch.CmdBindDescriptorSets(cb.handle(),
			pipelineBindPoint,
			cmd.pipeLayout->handle,
			firstSet,
			descriptorSetCount,
			setHandles.data(),
			dynamicOffsetCount,
			pDynamicOffsets);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdBindIndexBuffer(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    buffer,
		VkDeviceSize                                offset,
		VkIndexType                                 indexType) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<BindIndexBufferCmd>(cb);

	auto& buf = get(*cb.dev, buffer);
	cmd.buffer = &buf;
	useHandle(cb, cmd, buf);

	cmd.offset = offset;
	cmd.indexType = indexType;

	cb.graphicsState().indices.buffer = &buf;
	cb.graphicsState().indices.offset = offset;
	cb.graphicsState().indices.type = indexType;

	cb.dev->dispatch.CmdBindIndexBuffer(cb.handle(), buf.handle, offset, indexType);
}

VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    firstBinding,
		uint32_t                                    bindingCount,
		const VkBuffer*                             pBuffers,
		const VkDeviceSize*                         pOffsets) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<BindVertexBuffersCmd>(cb);
	cmd.firstBinding = firstBinding;

	ensureSize0(cb, cb.graphicsState().vertices, firstBinding + bindingCount);
	cmd.buffers = allocSpan<BoundVertexBuffer>(cb, bindingCount);
	auto bufHandles = LocalVector<VkBuffer>(bindingCount);
	for(auto i = 0u; i < bindingCount; ++i) {
		auto& buf = get(*cb.dev, pBuffers[i]);
		cmd.buffers[i].buffer = &buf;
		cmd.buffers[i].offset = pOffsets[i];
		useHandle(cb, cmd, buf);

		cb.graphicsState().vertices[firstBinding + i].buffer = &buf;
		cb.graphicsState().vertices[firstBinding + i].offset = pOffsets[i];

		bufHandles[i] = buf.handle;
	}

	cb.dev->dispatch.CmdBindVertexBuffers(cb.handle(),
		firstBinding, bindingCount, bufHandles.data(), pOffsets);
}

VKAPI_ATTR void VKAPI_CALL CmdDraw(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    vertexCount,
		uint32_t                                    instanceCount,
		uint32_t                                    firstVertex,
		uint32_t                                    firstInstance) {
	ExtZoneScoped;

	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<DrawCmd>(cb, cb, cb.graphicsState());

	cmd.vertexCount = vertexCount;
	cmd.instanceCount = instanceCount;
	cmd.firstVertex = firstVertex;
	cmd.firstInstance = firstInstance;

	{
		ExtZoneScopedN("dispatch");
		cb.dev->dispatch.CmdDraw(cb.handle(),
			vertexCount, instanceCount, firstVertex, firstInstance);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    indexCount,
		uint32_t                                    instanceCount,
		uint32_t                                    firstIndex,
		int32_t                                     vertexOffset,
		uint32_t                                    firstInstance) {
	ExtZoneScoped;

	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<DrawIndexedCmd>(cb, cb, cb.graphicsState());

	cmd.firstInstance = firstInstance;
	cmd.instanceCount = instanceCount;
	cmd.indexCount = indexCount;
	cmd.vertexOffset = vertexOffset;
	cmd.firstIndex = firstIndex;

	{
		ExtZoneScopedN("dispatch");
		cb.dev->dispatch.CmdDrawIndexed(cb.handle(),
			indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndirect(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    buffer,
		VkDeviceSize                                offset,
		uint32_t                                    drawCount,
		uint32_t                                    stride) {
	ExtZoneScoped;

	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<DrawIndirectCmd>(cb, cb, cb.graphicsState());

	auto& buf = cb.dev->buffers.get(buffer);
	cmd.buffer = &buf;
	useHandle(cb, cmd, buf);

	cmd.indexed = false;
	cmd.offset = offset;
	cmd.drawCount = drawCount;
	cmd.stride = stride;

	{
		ExtZoneScopedN("dispatch");
		cb.dev->dispatch.CmdDrawIndirect(cb.handle(),
			buf.handle, offset, drawCount, stride);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirect(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    buffer,
		VkDeviceSize                                offset,
		uint32_t                                    drawCount,
		uint32_t                                    stride) {
	ExtZoneScoped;

	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<DrawIndirectCmd>(cb, cb, cb.graphicsState());

	auto& buf = cb.dev->buffers.get(buffer);
	cmd.buffer = &buf;
	useHandle(cb, cmd, buf);

	cmd.indexed = true;
	cmd.offset = offset;
	cmd.drawCount = drawCount;
	cmd.stride = stride;

	{
		ExtZoneScopedN("dispatch");
		cb.dev->dispatch.CmdDrawIndexedIndirect(cb.handle(),
			buf.handle, offset, drawCount, stride);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndirectCount(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    buffer,
		VkDeviceSize                                offset,
		VkBuffer                                    countBuffer,
		VkDeviceSize                                countBufferOffset,
		uint32_t                                    maxDrawCount,
		uint32_t                                    stride) {
	ExtZoneScoped;

	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<DrawIndirectCountCmd>(cb, cb, cb.graphicsState());

	auto& buf = get(*cb.dev, buffer);
	cmd.buffer = &buf;
	useHandle(cb, cmd, buf);

	auto& countBuf = get(*cb.dev, countBuffer);
	cmd.countBuffer = &countBuf;
	useHandle(cb, cmd, countBuf);

	cmd.indexed = false;
	cmd.offset = offset;
	cmd.countBufferOffset = countBufferOffset;
	cmd.maxDrawCount = maxDrawCount;
	cmd.stride = stride;

	{
		ExtZoneScopedN("dispatch");
		cb.dev->dispatch.CmdDrawIndirectCount(cb.handle(), buf.handle, offset,
			countBuf.handle, countBufferOffset, maxDrawCount, stride);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirectCount(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    buffer,
		VkDeviceSize                                offset,
		VkBuffer                                    countBuffer,
		VkDeviceSize                                countBufferOffset,
		uint32_t                                    maxDrawCount,
		uint32_t                                    stride) {
	ExtZoneScoped;

	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<DrawIndirectCountCmd>(cb, cb, cb.graphicsState());

	auto& buf = get(*cb.dev, buffer);
	cmd.buffer = &buf;
	useHandle(cb, cmd, buf);

	auto& countBuf = get(*cb.dev, countBuffer);
	cmd.countBuffer = &countBuf;
	useHandle(cb, cmd, countBuf);

	cmd.indexed = true;
	cmd.offset = offset;
	cmd.countBufferOffset = countBufferOffset;
	cmd.maxDrawCount = maxDrawCount;
	cmd.stride = stride;

	{
		ExtZoneScopedN("dispatch");
		cb.dev->dispatch.CmdDrawIndexedIndirectCount(cb.handle(), buf.handle,
			offset, countBuf.handle, countBufferOffset, maxDrawCount, stride);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdDispatch(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    groupCountX,
		uint32_t                                    groupCountY,
		uint32_t                                    groupCountZ) {
	ExtZoneScoped;

	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<DispatchCmd>(cb, cb, cb.computeState());

	cmd.groupsX = groupCountX;
	cmd.groupsY = groupCountY;
	cmd.groupsZ = groupCountZ;

	{
		ExtZoneScopedN("dispatch");
		cb.dev->dispatch.CmdDispatch(cb.handle(), groupCountX, groupCountY, groupCountZ);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdDispatchIndirect(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    buffer,
		VkDeviceSize                                offset) {
	ExtZoneScoped;

	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<DispatchIndirectCmd>(cb, cb, cb.computeState());
	cmd.offset = offset;

	auto& buf = cb.dev->buffers.get(buffer);
	cmd.buffer = &buf;
	useHandle(cb, cmd, buf);

	{
		ExtZoneScopedN("dispatch");
		cb.dev->dispatch.CmdDispatchIndirect(cb.handle(), buf.handle, offset);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdDispatchBase(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    baseGroupX,
		uint32_t                                    baseGroupY,
		uint32_t                                    baseGroupZ,
		uint32_t                                    groupCountX,
		uint32_t                                    groupCountY,
		uint32_t                                    groupCountZ) {
	ExtZoneScoped;

	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<DispatchBaseCmd>(cb, cb, cb.computeState());

	cmd.baseGroupX = baseGroupX;
	cmd.baseGroupY = baseGroupY;
	cmd.baseGroupZ = baseGroupZ;
	cmd.groupsX = groupCountX;
	cmd.groupsY = groupCountY;
	cmd.groupsZ = groupCountZ;

	{
		ExtZoneScopedN("dispatch");
		cb.dev->dispatch.CmdDispatchBase(cb.handle(),
			baseGroupX, baseGroupY, baseGroupZ,
			groupCountX, groupCountY, groupCountZ);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdCopyImage(
		VkCommandBuffer                             commandBuffer,
		VkImage                                     srcImage,
		VkImageLayout                               srcImageLayout,
		VkImage                                     dstImage,
		VkImageLayout                               dstImageLayout,
		uint32_t                                    regionCount,
		const VkImageCopy*                          pRegions) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<CopyImageCmd>(cb);

	auto& src = cb.dev->images.get(srcImage);
	auto& dst = cb.dev->images.get(dstImage);

	cmd.src = &src;
	cmd.srcLayout = srcImageLayout;
	cmd.dst = &dst;
	cmd.dstLayout = dstImageLayout;
	upgradeSpan(cb, cmd.copies, pRegions, regionCount);

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

	cb.dev->dispatch.CmdCopyImage(cb.handle(),
		src.handle, srcImageLayout,
		dst.handle, dstImageLayout,
		regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyImage2KHR(
		VkCommandBuffer                             commandBuffer,
		const VkCopyImageInfo2KHR*                  info) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<CopyImageCmd>(cb);

	auto& src = cb.dev->images.get(info->srcImage);
	auto& dst = cb.dev->images.get(info->dstImage);

	cmd.pNext = copyChain(cb, info->pNext);
	cmd.src = &src;
	cmd.srcLayout = info->srcImageLayout;
	cmd.dst = &dst;
	cmd.dstLayout = info->dstImageLayout;
	cmd.copies = copySpan(cb, info->pRegions, info->regionCount);

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

	auto copy = *info;
	copy.dstImage = dst.handle;
	copy.srcImage = src.handle;
	cb.dev->dispatch.CmdCopyImage2KHR(cb.handle(), &copy);
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
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<BlitImageCmd>(cb);

	auto& src = cb.dev->images.get(srcImage);
	auto& dst = cb.dev->images.get(dstImage);

	cmd.src = &src;
	cmd.srcLayout = srcImageLayout;
	cmd.dst = &dst;
	cmd.dstLayout = dstImageLayout;
	upgradeSpan(cb, cmd.blits, pRegions, regionCount);
	cmd.filter = filter;

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

	cb.dev->dispatch.CmdBlitImage(cb.handle(),
		src.handle, srcImageLayout,
		dst.handle, dstImageLayout,
		regionCount, pRegions, filter);
}

VKAPI_ATTR void VKAPI_CALL CmdBlitImage2KHR(
		VkCommandBuffer                             commandBuffer,
		const VkBlitImageInfo2KHR*                  info) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<BlitImageCmd>(cb);

	auto& src = cb.dev->images.get(info->srcImage);
	auto& dst = cb.dev->images.get(info->dstImage);

	cmd.pNext = copyChain(cb, info->pNext);
	cmd.src = &src;
	cmd.srcLayout = info->srcImageLayout;
	cmd.dst = &dst;
	cmd.dstLayout = info->dstImageLayout;
	cmd.blits = copySpan(cb, info->pRegions, info->regionCount);
	cmd.filter = info->filter;

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

	auto copy = *info;
	copy.srcImage = src.handle;
	copy.dstImage = dst.handle;
	cb.dev->dispatch.CmdBlitImage2KHR(cb.handle(), &copy);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyBufferToImage(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    srcBuffer,
		VkImage                                     dstImage,
		VkImageLayout                               dstImageLayout,
		uint32_t                                    regionCount,
		const VkBufferImageCopy*                    pRegions) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<CopyBufferToImageCmd>(cb);

	auto& src = cb.dev->buffers.get(srcBuffer);
	auto& dst = cb.dev->images.get(dstImage);

	cmd.src = &src;
	cmd.dst = &dst;
	cmd.dstLayout = dstImageLayout;
	upgradeSpan(cb, cmd.copies, pRegions, regionCount);

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

	cb.dev->dispatch.CmdCopyBufferToImage(cb.handle(),
		src.handle, dst.handle, dstImageLayout, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyBufferToImage2KHR(
		VkCommandBuffer                             commandBuffer,
		const VkCopyBufferToImageInfo2KHR*          info) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<CopyBufferToImageCmd>(cb);

	auto& src = cb.dev->buffers.get(info->srcBuffer);
	auto& dst = cb.dev->images.get(info->dstImage);

	cmd.pNext = copyChain(cb, info->pNext);
	cmd.src = &src;
	cmd.dst = &dst;
	cmd.dstLayout = info->dstImageLayout;
	cmd.copies = copySpan(cb, info->pRegions, info->regionCount);

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

	auto copy = *info;
	copy.srcBuffer = src.handle;
	copy.dstImage = dst.handle;
	cb.dev->dispatch.CmdCopyBufferToImage2KHR(cb.handle(), &copy);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyImageToBuffer(
		VkCommandBuffer                             commandBuffer,
		VkImage                                     srcImage,
		VkImageLayout                               srcImageLayout,
		VkBuffer                                    dstBuffer,
		uint32_t                                    regionCount,
		const VkBufferImageCopy*                    pRegions) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<CopyImageToBufferCmd>(cb);

	auto& src = cb.dev->images.get(srcImage);
	auto& dst = cb.dev->buffers.get(dstBuffer);

	cmd.src = &src;
	cmd.dst = &dst;
	cmd.srcLayout = srcImageLayout;
	upgradeSpan(cb, cmd.copies, pRegions, regionCount);

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

	cb.dev->dispatch.CmdCopyImageToBuffer(cb.handle(),
		src.handle, srcImageLayout, dst.handle, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyImageToBuffer2KHR(
		VkCommandBuffer                             commandBuffer,
		const VkCopyImageToBufferInfo2KHR*          info) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<CopyImageToBufferCmd>(cb);

	auto& src = cb.dev->images.get(info->srcImage);
	auto& dst = cb.dev->buffers.get(info->dstBuffer);

	cmd.pNext = copyChain(cb, info->pNext);
	cmd.src = &src;
	cmd.dst = &dst;
	cmd.srcLayout = info->srcImageLayout;
	cmd.copies = copySpan(cb, info->pRegions, info->regionCount);

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

	auto copy = *info;
	copy.dstBuffer = dst.handle;
	copy.srcImage = src.handle;
	cb.dev->dispatch.CmdCopyImageToBuffer2KHR(cb.handle(), &copy);
}

VKAPI_ATTR void VKAPI_CALL CmdClearColorImage(
		VkCommandBuffer                             commandBuffer,
		VkImage                                     image,
		VkImageLayout                               imageLayout,
		const VkClearColorValue*                    pColor,
		uint32_t                                    rangeCount,
		const VkImageSubresourceRange*              pRanges) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<ClearColorImageCmd>(cb);

	auto& dst = cb.dev->images.get(image);
	cmd.dst = &dst;
	cmd.color = *pColor;
	cmd.dstLayout = imageLayout;
	cmd.ranges = copySpan(cb, pRanges, rangeCount);

	useHandle(cb, cmd, dst);

	cb.dev->dispatch.CmdClearColorImage(cb.handle(),
		dst.handle, imageLayout, pColor, rangeCount, pRanges);
}

VKAPI_ATTR void VKAPI_CALL CmdClearDepthStencilImage(
		VkCommandBuffer                             commandBuffer,
		VkImage                                     image,
		VkImageLayout                               imageLayout,
		const VkClearDepthStencilValue*             pDepthStencil,
		uint32_t                                    rangeCount,
		const VkImageSubresourceRange*              pRanges) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<ClearDepthStencilImageCmd>(cb);

	auto& dst = cb.dev->images.get(image);
	cmd.dst = &dst;
	cmd.dstLayout = imageLayout;
	cmd.value = *pDepthStencil;
	cmd.ranges = copySpan(cb, pRanges, rangeCount);

	useHandle(cb, cmd, dst);

	cb.dev->dispatch.CmdClearDepthStencilImage(cb.handle(), dst.handle,
		imageLayout, pDepthStencil, rangeCount, pRanges);
}

VKAPI_ATTR void VKAPI_CALL CmdClearAttachments(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    attachmentCount,
		const VkClearAttachment*                    pAttachments,
		uint32_t                                    rectCount,
		const VkClearRect*                          pRects) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<ClearAttachmentCmd>(cb);

	cmd.attachments = copySpan(cb, pAttachments, attachmentCount);
	cmd.rects = copySpan(cb, pRects, rectCount);

	dlg_assert(cb.graphicsState().rpi.rp);
	cmd.rpi = cb.graphicsState().rpi;

	// NOTE: add clears attachments handles to used handles for this cmd?
	// but they were already used in BeginRenderPass and are just implicitly
	// used here (as in many other render pass cmds). So we probably should
	// not do it.

	cb.dev->dispatch.CmdClearAttachments(cb.handle(), attachmentCount,
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
	auto& cb = getCommandBuffer(commandBuffer);
	auto& src = cb.dev->images.get(srcImage);
	auto& dst = cb.dev->images.get(dstImage);

	auto& cmd = addCmd<ResolveImageCmd>(cb);
	cmd.src = &src;
	cmd.srcLayout = srcImageLayout;
	cmd.dst = &dst;
	cmd.dstLayout = dstImageLayout;
	upgradeSpan(cb, cmd.regions, pRegions, regionCount);

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

	cb.dev->dispatch.CmdResolveImage(cb.handle(), src.handle, srcImageLayout,
		dst.handle, dstImageLayout, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL CmdResolveImage2KHR(
		VkCommandBuffer                             commandBuffer,
		const VkResolveImageInfo2KHR*               info) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& src = cb.dev->images.get(info->srcImage);
	auto& dst = cb.dev->images.get(info->dstImage);

	auto& cmd = addCmd<ResolveImageCmd>(cb);

	cmd.pNext = copyChain(cb, info->pNext);
	cmd.src = &src;
	cmd.srcLayout = info->srcImageLayout;
	cmd.dst = &dst;
	cmd.dstLayout = info->dstImageLayout;
	cmd.regions = copySpan(cb, info->pRegions, info->regionCount);

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

	auto copy = *info;
	copy.dstImage = dst.handle;
	copy.srcImage = src.handle;
	cb.dev->dispatch.CmdResolveImage2KHR(cb.handle(), &copy);
}

VKAPI_ATTR void VKAPI_CALL CmdSetEvent(
		VkCommandBuffer                             commandBuffer,
		VkEvent                                     event,
		VkPipelineStageFlags                        stageMask) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetEventCmd>(cb);
	cmd.event = &cb.dev->events.get(event);
	cmd.stageMask = stageMask;

	useHandle(cb, cmd, *cmd.event);

	cb.dev->dispatch.CmdSetEvent(cb.handle(), cmd.event->handle, stageMask);
}

VKAPI_ATTR void VKAPI_CALL CmdResetEvent(
		VkCommandBuffer                             commandBuffer,
		VkEvent                                     event,
		VkPipelineStageFlags                        stageMask) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<ResetEventCmd>(cb);
	cmd.event = &cb.dev->events.get(event);
	cmd.stageMask = stageMask;

	useHandle(cb, cmd, *cmd.event);

	cb.dev->dispatch.CmdSetEvent(cb.handle(), cmd.event->handle, stageMask);
}

VKAPI_ATTR void VKAPI_CALL CmdExecuteCommands(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    commandBufferCount,
		const VkCommandBuffer*                      pCommandBuffers) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<ExecuteCommandsCmd>(cb);
	auto* last = cmd.children_; // nullptr

	auto cbHandles = LocalVector<VkCommandBuffer>(commandBufferCount);
	for(auto i = 0u; i < commandBufferCount; ++i) {
		auto& secondary = getCommandBuffer(pCommandBuffers[i]);
		dlg_assert(secondary.state() == CommandBuffer::State::executable);

		// We don't have to lock the mutex here because the command buffer
		// state is not allowed to change while it is used here.
		auto recordPtr = secondary.lastRecordPtrLocked();

		auto& childCmd = vil::allocate<ExecuteCommandsChildCmd>(cb);
		childCmd.id_ = i;
		childCmd.record_ = recordPtr.get();

		if(!last) {
			dlg_assert(!cmd.children_);
			cmd.children_ = &childCmd;
		} else {
			dlg_assert(cmd.children_);
			last->next = &childCmd;
		}

		cb.record()->secondaries.push_back(std::move(recordPtr));

		// Needed to correctly invalidate cb when a secondary buffer is
		// reset/destroyed.
		useHandle(cb, cmd, secondary);

		auto& rec = *secondary.record();
		for(auto& img : rec.images) {
			if(img.second.layoutChanged) {
				useHandle(cb, cmd, *img.second.image, img.second.finalLayout);
			} else {
				useHandle(cb, cmd, *img.second.image);
			}
		}

		for(auto& handle : rec.handles) {
			useHandle(cb, cmd, handle.first, *handle.second.handle);
		}

		cbHandles[i] = secondary.handle();
		last = &childCmd;
	}

	cb.dev->dispatch.CmdExecuteCommands(cb.handle(),
		commandBufferCount, cbHandles.data());
}

VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    srcBuffer,
		VkBuffer                                    dstBuffer,
		uint32_t                                    regionCount,
		const VkBufferCopy*                         pRegions) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<CopyBufferCmd>(cb);

	auto& srcBuf = get(*cb.dev, srcBuffer);
	auto& dstBuf = get(*cb.dev, dstBuffer);

	cmd.src = &srcBuf;
	cmd.dst = &dstBuf;
	upgradeSpan(cb, cmd.regions, pRegions, regionCount);

	useHandle(cb, cmd, srcBuf);
	useHandle(cb, cmd, dstBuf);

	cb.dev->dispatch.CmdCopyBuffer(cb.handle(),
		srcBuf.handle, dstBuf.handle, regionCount, pRegions);
}


VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer2KHR(
		VkCommandBuffer                             commandBuffer,
		const VkCopyBufferInfo2KHR*                 info) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<CopyBufferCmd>(cb);

	auto& srcBuf = get(*cb.dev, info->srcBuffer);
	auto& dstBuf = get(*cb.dev, info->dstBuffer);

	cmd.pNext = copyChain(cb, info->pNext);
	cmd.src = &srcBuf;
	cmd.dst = &dstBuf;
	cmd.regions = copySpan(cb, info->pRegions, info->regionCount);

	useHandle(cb, cmd, srcBuf);
	useHandle(cb, cmd, dstBuf);

	auto copy = *info;
	copy.srcBuffer = srcBuf.handle;
	copy.dstBuffer = dstBuf.handle;

	cb.dev->dispatch.CmdCopyBuffer2KHR(cb.handle(), &copy);
}

VKAPI_ATTR void VKAPI_CALL CmdUpdateBuffer(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    dstBuffer,
		VkDeviceSize                                dstOffset,
		VkDeviceSize                                dataSize,
		const void*                                 pData) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<UpdateBufferCmd>(cb);

	auto& buf = get(*cb.dev, dstBuffer);
	auto dataPtr = static_cast<const std::byte*>(pData);
	cmd.data = copySpan(cb, static_cast<const std::byte*>(dataPtr), dataSize);
	cmd.dst = &buf;
	cmd.offset = dstOffset;

	useHandle(cb, cmd, buf);

	cb.dev->dispatch.CmdUpdateBuffer(cb.handle(), buf.handle, dstOffset, dataSize, pData);
}

VKAPI_ATTR void VKAPI_CALL CmdFillBuffer(
		VkCommandBuffer                             commandBuffer,
		VkBuffer                                    dstBuffer,
		VkDeviceSize                                dstOffset,
		VkDeviceSize                                size,
		uint32_t                                    data) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<FillBufferCmd>(cb);

	auto& buf = get(*cb.dev, dstBuffer);
	cmd.dst = &buf;
	cmd.offset = dstOffset;
	cmd.size = size;
	cmd.data = data;

	useHandle(cb, cmd, buf);

	cb.dev->dispatch.CmdFillBuffer(cb.handle(), buf.handle, dstOffset, size, data);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginDebugUtilsLabelEXT(
		VkCommandBuffer                             commandBuffer,
		const VkDebugUtilsLabelEXT*                 pLabelInfo) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<BeginDebugUtilsLabelCmd, SectionType::begin>(cb);

	auto* c = pLabelInfo->color;
	cmd.color = {c[0], c[1], c[2], c[3]};
	cmd.name = copyString(cb, pLabelInfo->pLabelName);
	// TODO: copy pNext?

	if(cb.dev->dispatch.CmdBeginDebugUtilsLabelEXT) {
		cb.dev->dispatch.CmdBeginDebugUtilsLabelEXT(cb.handle(), pLabelInfo);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdEndDebugUtilsLabelEXT(
		VkCommandBuffer                             commandBuffer) {
	auto& cb = getCommandBuffer(commandBuffer);

	// See docs/debug-utils-label-nesting.md
	// When the last section isn't a BeginDebugUtilsLabelCmd, we have to
	// find the last one in the cb.section_ stack and mark it for pop.
	if(cb.ignoreEndDebugLabels() > 0) {
		--cb.ignoreEndDebugLabels();
	} else if(cb.section() && dynamic_cast<BeginDebugUtilsLabelCmd*>(cb.section()->cmd)) {
		cb.endSection(nullptr);
	} else {
		auto* it = cb.section();
		while(it) {
			auto lcmd = dynamic_cast<BeginDebugUtilsLabelCmd*>(it->cmd);
			if(lcmd && !it->pop) {
				dlg_trace("Problematic debug utils label nesting detected (End)");
				it->pop = true;
				break;
			}

			it = it->parent;
		}

		// If there is no active label section at all, the command buffer
		// effectively pops it from the queue.
		if(!it) {
			++cb.record()->numPopLabels;
		}
	}

	// Create without SectionType::end, we ended the section above
	// manually if possible.
	addCmd<EndDebugUtilsLabelCmd>(cb);

	if(cb.dev->dispatch.CmdEndDebugUtilsLabelEXT) {
		cb.dev->dispatch.CmdEndDebugUtilsLabelEXT(cb.handle());
	}
}

VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(
		VkCommandBuffer                             commandBuffer,
		VkPipelineBindPoint                         pipelineBindPoint,
		VkPipeline                                  pipeline) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<BindPipelineCmd>(cb);
	cmd.bindPoint = pipelineBindPoint;

	auto& pipe = get(*cb.dev, pipeline);
	dlg_assert(pipe.type == pipelineBindPoint);

	if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
		cb.computeState().pipe = static_cast<ComputePipeline*>(&pipe);
		cmd.pipe = cb.computeState().pipe;
		useHandle(cb, cmd, *cmd.pipe);
	} else if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
		cb.graphicsState().pipe = static_cast<GraphicsPipeline*>(&pipe);
		cmd.pipe = cb.graphicsState().pipe;
		useHandle(cb, cmd, *cmd.pipe);
	} else if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) {
		cb.rayTracingState().pipe = static_cast<RayTracingPipeline*>(&pipe);
		cmd.pipe = cb.rayTracingState().pipe;
		useHandle(cb, cmd, *cmd.pipe);
	} else {
		dlg_error("unknown pipeline bind point");
	}

	cb.dev->dispatch.CmdBindPipeline(cb.handle(), pipelineBindPoint, pipe.handle);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginQuery(
		VkCommandBuffer                             commandBuffer,
		VkQueryPool                                 queryPool,
		uint32_t                                    query,
		VkQueryControlFlags                         flags) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<BeginQueryCmd>(cb);
	cmd.pool = &cb.dev->queryPools.get(queryPool);
	cmd.query = query;
	cmd.flags = flags;

	useHandle(cb, cmd, *cmd.pool);

	cb.dev->dispatch.CmdBeginQuery(cb.handle(), cmd.pool->handle, query, flags);
}

VKAPI_ATTR void VKAPI_CALL CmdEndQuery(
		VkCommandBuffer                             commandBuffer,
		VkQueryPool                                 queryPool,
		uint32_t                                    query) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<EndQueryCmd>(cb);
	cmd.pool = &cb.dev->queryPools.get(queryPool);
	cmd.query = query;

	useHandle(cb, cmd, *cmd.pool);

	cb.dev->dispatch.CmdEndQuery(cb.handle(), cmd.pool->handle, query);
}

VKAPI_ATTR void VKAPI_CALL CmdResetQueryPool(
		VkCommandBuffer                             commandBuffer,
		VkQueryPool                                 queryPool,
		uint32_t                                    firstQuery,
		uint32_t                                    queryCount) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<ResetQueryPoolCmd>(cb);
	cmd.pool = &cb.dev->queryPools.get(queryPool);
	cmd.first = firstQuery;
	cmd.count = queryCount;

	useHandle(cb, cmd, *cmd.pool);

	cb.dev->dispatch.CmdResetQueryPool(cb.handle(), cmd.pool->handle, firstQuery, queryCount);
}

VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp(
		VkCommandBuffer                             commandBuffer,
		VkPipelineStageFlagBits                     pipelineStage,
		VkQueryPool                                 queryPool,
		uint32_t                                    query) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<WriteTimestampCmd>(cb);
	cmd.pool = &cb.dev->queryPools.get(queryPool);
	cmd.stage = pipelineStage;
	cmd.query = query;

	useHandle(cb, cmd, *cmd.pool);

	cb.dev->dispatch.CmdWriteTimestamp(cb.handle(), pipelineStage, cmd.pool->handle, query);
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
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<CopyQueryPoolResultsCmd>(cb);
	cmd.pool = &cb.dev->queryPools.get(queryPool);
	cmd.first = firstQuery;
	cmd.count = queryCount;
	cmd.dstBuffer = &get(*cb.dev, dstBuffer);
	cmd.dstOffset = dstOffset;
	cmd.stride = stride;
	cmd.flags = flags;

	useHandle(cb, cmd, *cmd.pool);
	useHandle(cb, cmd, *cmd.dstBuffer);

	cb.dev->dispatch.CmdCopyQueryPoolResults(cb.handle(), cmd.pool->handle,
		firstQuery, queryCount, cmd.dstBuffer->handle, dstOffset, stride, flags);
}

VKAPI_ATTR void VKAPI_CALL CmdPushConstants(
		VkCommandBuffer                             commandBuffer,
		VkPipelineLayout                            pipeLayout,
		VkShaderStageFlags                          stageFlags,
		uint32_t                                    offset,
		uint32_t                                    size,
		const void*                                 pValues) {
	ExtZoneScoped;

	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<PushConstantsCmd>(cb);

	// NOTE: See BindDescriptorSets for rationale on pipe layout handling here.
	auto pipeLayoutPtr = getPtr(*cb.dev, pipeLayout);
	cmd.pipeLayout = pipeLayoutPtr.get();
	cb.record()->pipeLayouts.emplace_back(std::move(pipeLayoutPtr));

	cmd.stages = stageFlags;
	cmd.offset = offset;
	auto ptr = static_cast<const std::byte*>(pValues);
	cmd.values = copySpan(cb, static_cast<const std::byte*>(ptr), size);

	ensureSize0(cb, cb.pushConstants().data, std::max(offset + size, 128u));
	std::memcpy(cb.pushConstants().data.data() + offset, pValues, size);

	// TODO: improve pcr tracking
	// not sure about this. The spec isn't clear about this.
	// But this seems to be what the validation layers do.
	// https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/57f6f2a387b37c442c4db6993eb064a1e750b30f/layers/state_tracker.cpp#L5868
	// if(cb.pushConstants.layout &&
	// 		!pushConstantCompatible(*cmd.layout, *cb.pushConstants.layout)) {
	// 	cb.pushConstants.layout = nullptr;
	// 	cb.pushConstants.map.clear();
	// }
	// cb.pushConstants.layout = cmd.layout.get();
	/*
	for(auto i = 0u; i < 32; ++i) {
		if((stageFlags & (1 << i)) == 0) {
			continue;
		}

		auto& pc = cb.pushConstants.map[VkShaderStageFlagBits(1 << i)];
		ensureSize(cb, pc.data, offset + size);
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
	*/

	{
		ExtZoneScopedN("dispatch");
		cb.dev->dispatch.CmdPushConstants(cb.handle(), cmd.pipeLayout->handle,
			stageFlags, offset, size, pValues);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdSetViewport(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    firstViewport,
		uint32_t                                    viewportCount,
		const VkViewport*                           pViewports) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetViewportCmd>(cb);
	cmd.first = firstViewport;
	cmd.viewports = copySpan(cb, pViewports, viewportCount);

	ensureSize0(cb, cb.graphicsState().dynamic.viewports, firstViewport + viewportCount);
	std::copy(pViewports, pViewports + viewportCount,
		cb.graphicsState().dynamic.viewports.begin() + firstViewport);

	cb.dev->dispatch.CmdSetViewport(cb.handle(), firstViewport, viewportCount, pViewports);
}

VKAPI_ATTR void VKAPI_CALL CmdSetScissor(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    firstScissor,
		uint32_t                                    scissorCount,
		const VkRect2D*                             pScissors) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetScissorCmd>(cb);
	cmd.first = firstScissor;
	cmd.scissors = copySpan(cb, pScissors, scissorCount);

	ensureSize0(cb, cb.graphicsState().dynamic.scissors, firstScissor + scissorCount);
	std::copy(pScissors, pScissors + scissorCount,
		cb.graphicsState().dynamic.scissors.begin() + firstScissor);

	cb.dev->dispatch.CmdSetScissor(cb.handle(), firstScissor, scissorCount, pScissors);
}

VKAPI_ATTR void VKAPI_CALL CmdSetLineWidth(
		VkCommandBuffer                             commandBuffer,
		float                                       lineWidth) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetLineWidthCmd>(cb);
	cmd.width = lineWidth;

	cb.graphicsState().dynamic.lineWidth = lineWidth;
	cb.dev->dispatch.CmdSetLineWidth(cb.handle(), lineWidth);
}

VKAPI_ATTR void VKAPI_CALL CmdSetDepthBias(
		VkCommandBuffer                             commandBuffer,
		float                                       depthBiasConstantFactor,
		float                                       depthBiasClamp,
		float                                       depthBiasSlopeFactor) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetDepthBiasCmd>(cb);
	cmd.state = {depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor};
	cb.graphicsState().dynamic.depthBias = cmd.state;

	cb.dev->dispatch.CmdSetDepthBias(cb.handle(),
		depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
}

VKAPI_ATTR void VKAPI_CALL CmdSetBlendConstants(
		VkCommandBuffer                             commandBuffer,
		const float                                 blendConstants[4]) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetBlendConstantsCmd>(cb);
	std::memcpy(cmd.values.data(), blendConstants, sizeof(cmd.values));
	std::memcpy(cb.graphicsState().dynamic.blendConstants.data(), blendConstants,
		sizeof(cb.graphicsState().dynamic.blendConstants));

	cb.dev->dispatch.CmdSetBlendConstants(cb.handle(), blendConstants);
}

VKAPI_ATTR void VKAPI_CALL CmdSetDepthBounds(
		VkCommandBuffer                             commandBuffer,
		float                                       minDepthBounds,
		float                                       maxDepthBounds) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetDepthBoundsCmd>(cb);
	cmd.min = minDepthBounds;
	cmd.max = maxDepthBounds;
	cb.graphicsState().dynamic.depthBoundsMin = minDepthBounds;
	cb.graphicsState().dynamic.depthBoundsMax = maxDepthBounds;

	cb.dev->dispatch.CmdSetDepthBounds(cb.handle(), minDepthBounds, maxDepthBounds);
}

VKAPI_ATTR void VKAPI_CALL CmdSetStencilCompareMask(
		VkCommandBuffer                             commandBuffer,
		VkStencilFaceFlags                          faceMask,
		uint32_t                                    compareMask) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetStencilCompareMaskCmd>(cb);
	cmd.faceMask = faceMask;
	cmd.value = compareMask;
	if(faceMask & VK_STENCIL_FACE_FRONT_BIT) {
		cb.graphicsState().dynamic.stencilFront.compareMask = compareMask;
	}
	if(faceMask & VK_STENCIL_FACE_BACK_BIT) {
		cb.graphicsState().dynamic.stencilBack.compareMask = compareMask;
	}

	cb.dev->dispatch.CmdSetStencilCompareMask(cb.handle(), faceMask, compareMask);
}

VKAPI_ATTR void VKAPI_CALL CmdSetStencilWriteMask(
		VkCommandBuffer                             commandBuffer,
		VkStencilFaceFlags                          faceMask,
		uint32_t                                    writeMask) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetStencilWriteMaskCmd>(cb);
	cmd.faceMask = faceMask;
	cmd.value = writeMask;
	if(faceMask & VK_STENCIL_FACE_FRONT_BIT) {
		cb.graphicsState().dynamic.stencilFront.compareMask = writeMask;
	}
	if(faceMask & VK_STENCIL_FACE_BACK_BIT) {
		cb.graphicsState().dynamic.stencilBack.compareMask = writeMask;
	}

	cb.dev->dispatch.CmdSetStencilWriteMask(cb.handle(), faceMask, writeMask);
}

VKAPI_ATTR void VKAPI_CALL CmdSetStencilReference(
		VkCommandBuffer                             commandBuffer,
		VkStencilFaceFlags                          faceMask,
		uint32_t                                    reference) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetStencilReferenceCmd>(cb);
	cmd.faceMask = faceMask;
	cmd.value = reference;
	if(faceMask & VK_STENCIL_FACE_FRONT_BIT) {
		cb.graphicsState().dynamic.stencilFront.reference = reference;
	}
	if(faceMask & VK_STENCIL_FACE_BACK_BIT) {
		cb.graphicsState().dynamic.stencilBack.reference = reference;
	}

	cb.dev->dispatch.CmdSetStencilReference(cb.handle(), faceMask, reference);
}

VKAPI_ATTR void VKAPI_CALL CmdPushDescriptorSetKHR(
		VkCommandBuffer                             commandBuffer,
		VkPipelineBindPoint                         pipelineBindPoint,
		VkPipelineLayout                            layout,
		uint32_t                                    set,
		uint32_t                                    descriptorWriteCount,
		const VkWriteDescriptorSet*                 pDescriptorWrites) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<PushDescriptorSetCmd>(cb);
	cmd.bindPoint = pipelineBindPoint;
	cmd.set = set;
	cmd.descriptorWrites = copySpan(cb, pDescriptorWrites, descriptorWriteCount);

	// TODO: need to track the state in graphicsState/computeState.

	// deep-copy cmd.descriptorWrites.
	// The allocation done by copySpan is bound to the lifetime of the record
	// and we don't need to store the span itself anywhere since we don't
	// directly have ownership
	for(auto& write : cmd.descriptorWrites) {
		write.pNext = copyChain(cb, write.pNext);
		if(!write.descriptorCount) {
			continue;
		}

		switch(category(write.descriptorType)) {
			case DescriptorCategory::buffer: {
				dlg_assert(write.pBufferInfo);
				auto copies = allocSpan<VkDescriptorBufferInfo>(cb, write.descriptorCount);
				for(auto i = 0u; i < write.descriptorCount; ++i) {
					auto& buf = get(*cb.dev, write.pBufferInfo[i].buffer);
					copies[i] = write.pBufferInfo[i];
					copies[i].buffer = buf.handle;
					useHandle(cb, cmd, buf);
				}
				write.pBufferInfo = copies.data();
				break;
			} case DescriptorCategory::image: {
				dlg_assert(write.pImageInfo);
				auto copies = allocSpan<VkDescriptorImageInfo>(cb, write.descriptorCount);
				for(auto i = 0u; i < write.descriptorCount; ++i) {
					copies[i] = write.pImageInfo[i];
					if(copies[i].imageView) {
						auto& iv = get(*cb.dev, write.pImageInfo[i].imageView);
						copies[i].imageView = iv.handle;
						useHandle(cb, cmd, iv);
					}
					if(copies[i].sampler) {
						auto& sampler = get(*cb.dev, write.pImageInfo[i].sampler);
						copies[i].sampler = sampler.handle;
						useHandle(cb, cmd, sampler);
					}
				}
				write.pImageInfo = copies.data();
				break;
			} case DescriptorCategory::bufferView: {
				dlg_assert(write.pTexelBufferView);
				auto copies = allocSpan<VkBufferView>(cb, write.descriptorCount);
				for(auto i = 0u; i < write.descriptorCount; ++i) {
					auto& bv = get(*cb.dev, write.pTexelBufferView[i]);
					copies[i] = bv.handle;
					useHandle(cb, cmd, bv);
				}
				write.pTexelBufferView = copies.data();
				break;
			} default:
				dlg_error("Invalid/unknown descriptor type");
				break;
		}
	}

	auto pipeLayoutPtr = getPtr(*cb.dev, layout);
	cmd.pipeLayout = pipeLayoutPtr.get();
	cb.record()->pipeLayouts.emplace_back(std::move(pipeLayoutPtr));

	cb.dev->dispatch.CmdPushDescriptorSetKHR(cb.handle(),
		pipelineBindPoint, cmd.pipeLayout->handle, set,
		descriptorWriteCount, cmd.descriptorWrites.data());
}

VKAPI_ATTR void VKAPI_CALL CmdPushDescriptorSetWithTemplateKHR(
		VkCommandBuffer                             commandBuffer,
		VkDescriptorUpdateTemplate                  descriptorUpdateTemplate,
		VkPipelineLayout                            layout,
		uint32_t                                    set,
		const void*                                 pData) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<PushDescriptorSetWithTemplateCmd>(cb);
	cmd.set = set;

	// TODO: need to track the state in graphicsState/computeState.

	auto pipeLayoutPtr = getPtr(*cb.dev, layout);
	cmd.pipeLayout = pipeLayoutPtr.get();
	cb.record()->pipeLayouts.emplace_back(std::move(pipeLayoutPtr));

	dlg_assert(set < cmd.pipeLayout->descriptors.size());
	auto& dsLayout = *cmd.pipeLayout->descriptors[set];

	auto dsUpdateTemplate = getPtr(*cb.dev, descriptorUpdateTemplate);
	cmd.updateTemplate = dsUpdateTemplate.get();
	cb.record()->dsUpdateTemplates.emplace_back(std::move(dsUpdateTemplate));

	auto& dut = get(*cb.dev, descriptorUpdateTemplate);
	auto dataSize = totalUpdateDataSize(*cmd.updateTemplate);
	auto copied = copySpan(cb, reinterpret_cast<const std::byte*>(pData), dataSize);
	auto ptr = copied.data();

	for(auto& entry : dut.entries) {
		auto dstBinding = entry.dstBinding;
		auto dstElem = entry.dstArrayElement;
		for(auto j = 0u; j < entry.descriptorCount; ++j, ++dstElem) {
			// advanceUntilValid(?, dstBinding, dstElem);
			auto dsType = dsLayout.bindings[dstBinding].descriptorType;

			// TODO: such an assertion here would be nice. Track used
			// layout in update?
			// dlg_assert(write.descriptorType == type);

			auto* data = ptr + (entry.offset + j * entry.stride);

			// TODO: the reinterpret_cast here is UB in C++ I guess.
			// Assuming the caller did it correctly (really creating
			// the objects e.g. via placement new) we could probably also
			// do it correctly by using placement new (copy) into 'copied'
			// instead of the memcpy above.
			switch(category(dsType)) {
				case DescriptorCategory::image: {
					auto& imgInfo = *reinterpret_cast<VkDescriptorImageInfo*>(data);
					if(imgInfo.imageView) {
						auto& iv = get(*cb.dev, imgInfo.imageView);
						imgInfo.imageView = iv.handle;
						useHandle(cb, cmd, iv);
					}
					if(imgInfo.sampler) {
						auto& sampler = get(*cb.dev, imgInfo.sampler);
						imgInfo.sampler = sampler.handle;
						useHandle(cb, cmd, sampler);
					}
					break;
				} case DescriptorCategory::buffer: {
					auto& bufInfo = *reinterpret_cast<VkDescriptorBufferInfo*>(data);
					auto& buf = get(*cb.dev, bufInfo.buffer);
					bufInfo.buffer = buf.handle;
					useHandle(cb, cmd, buf);
					break;
				} case DescriptorCategory::bufferView: {
					auto& vkBufView = *reinterpret_cast<VkBufferView*>(data);
					auto& bv = get(*cb.dev, vkBufView);
					vkBufView = bv.handle;
					useHandle(cb, cmd, bv);
					break;
				} case DescriptorCategory::accelStruct: {
					auto& vkAccelStruct = *reinterpret_cast<VkAccelerationStructureKHR*>(data);
					auto& accelStruct = get(*cb.dev, vkAccelStruct);
					vkAccelStruct = accelStruct.handle;
					useHandle(cb, cmd, accelStruct);
					break;
				} case DescriptorCategory::inlineUniformBlock: {
					// nothing to do, no unwrapping needed
					break;
				} case DescriptorCategory::none:
					dlg_error("Invalid/unknown descriptor type");
					break;
			}
		}
	}

	cmd.data = copied;
	cb.dev->dispatch.CmdPushDescriptorSetWithTemplateKHR(cb.handle(),
		dut.handle, cmd.pipeLayout->handle, set, cmd.data.data());
}

VKAPI_ATTR void VKAPI_CALL CmdSetFragmentShadingRateKHR(
		VkCommandBuffer                             commandBuffer,
		const VkExtent2D*                           pFragmentSize,
		const VkFragmentShadingRateCombinerOpKHR    combinerOps[2]) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetFragmentShadingRateCmd>(cb);
	cmd.fragmentSize = *pFragmentSize;
	cmd.combinerOps = {combinerOps[0], combinerOps[1]};

	cb.dev->dispatch.CmdSetFragmentShadingRateKHR(cb.handle(), pFragmentSize, combinerOps);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginConditionalRenderingEXT(
		VkCommandBuffer                             commandBuffer,
		const VkConditionalRenderingBeginInfoEXT*   pConditionalRenderingBegin) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<BeginConditionalRenderingCmd, SectionType::begin>(cb);
	cmd.buffer = &get(*cb.dev, pConditionalRenderingBegin->buffer);
	cmd.offset = pConditionalRenderingBegin->offset;
	cmd.flags = pConditionalRenderingBegin->flags;

	useHandle(cb, cmd, *cmd.buffer);

	auto info = *pConditionalRenderingBegin;
	info.buffer = cmd.buffer->handle;

	cb.dev->dispatch.CmdBeginConditionalRenderingEXT(cb.handle(), &info);
}

VKAPI_ATTR void VKAPI_CALL CmdEndConditionalRenderingEXT(
		VkCommandBuffer                             commandBuffer) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<EndConditionalRenderingCmd, SectionType::end>(cb);
	(void) cmd;

	cb.dev->dispatch.CmdEndConditionalRenderingEXT(cb.handle());
}

VKAPI_ATTR void VKAPI_CALL CmdSetLineStippleEXT(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    lineStippleFactor,
		uint16_t                                    lineStipplePattern) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetLineStippleCmd>(cb);
	cmd.stippleFactor = lineStippleFactor;
	cmd.stipplePattern = lineStipplePattern;

	cb.dev->dispatch.CmdSetLineStippleEXT(cb.handle(), lineStippleFactor, lineStipplePattern);
}

VKAPI_ATTR void VKAPI_CALL CmdSetCullModeEXT(
		VkCommandBuffer                             commandBuffer,
		VkCullModeFlags                             cullMode) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetCullModeCmd>(cb);
	cmd.cullMode = cullMode;

	cb.dev->dispatch.CmdSetCullModeEXT(cb.handle(), cullMode);
}

VKAPI_ATTR void VKAPI_CALL CmdSetFrontFaceEXT(
		VkCommandBuffer                             commandBuffer,
		VkFrontFace                                 frontFace) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetFrontFaceCmd>(cb);
	cmd.frontFace = frontFace;

	cb.dev->dispatch.CmdSetCullModeEXT(cb.handle(), frontFace);
}

VKAPI_ATTR void VKAPI_CALL CmdSetPrimitiveTopologyEXT(
		VkCommandBuffer                             commandBuffer,
		VkPrimitiveTopology                         primitiveTopology) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetPrimitiveTopologyCmd>(cb);
	cmd.topology = primitiveTopology;

	cb.dev->dispatch.CmdSetPrimitiveTopologyEXT(cb.handle(), primitiveTopology);
}

VKAPI_ATTR void VKAPI_CALL CmdSetViewportWithCountEXT(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    viewportCount,
		const VkViewport*                           pViewports) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetViewportWithCountCmd>(cb);
	cmd.viewports = copySpan(cb, pViewports, viewportCount);

	cb.dev->dispatch.CmdSetViewportWithCountEXT(cb.handle(), viewportCount,
		pViewports);
}

VKAPI_ATTR void VKAPI_CALL CmdSetScissorWithCountEXT(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    scissorCount,
		const VkRect2D*                             pScissors) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetScissorWithCountCmd>(cb);
	cmd.scissors = copySpan(cb, pScissors, scissorCount);

	cb.dev->dispatch.CmdSetScissorWithCountEXT(cb.handle(), scissorCount,
		pScissors);
}

VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers2EXT(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    firstBinding,
		uint32_t                                    bindingCount,
		const VkBuffer*                             pBuffers,
		const VkDeviceSize*                         pOffsets,
		const VkDeviceSize*                         pSizes,
		const VkDeviceSize*                         pStrides) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<BindVertexBuffersCmd>(cb);
	cmd.firstBinding = firstBinding;

	ensureSize0(cb, cb.graphicsState().vertices, firstBinding + bindingCount);
	cmd.buffers = allocSpan<BoundVertexBuffer>(cb, bindingCount);
	auto bufHandles = LocalVector<VkBuffer>(bindingCount);
	for(auto i = 0u; i < bindingCount; ++i) {
		auto& buf = get(*cb.dev, pBuffers[i]);
		cmd.buffers[i].buffer = &buf;
		cmd.buffers[i].offset = pOffsets[i];
		cmd.buffers[i].size = pSizes ? pSizes[i] : 0u;
		cmd.buffers[i].stride = pStrides ? pStrides[i] : 0u;
		useHandle(cb, cmd, buf);

		cb.graphicsState().vertices[firstBinding + i] = cmd.buffers[i];
		bufHandles[i] = buf.handle;
	}

	cb.dev->dispatch.CmdBindVertexBuffers2EXT(cb.handle(),
		firstBinding, bindingCount, bufHandles.data(), pOffsets, pSizes,
		pStrides);
}

VKAPI_ATTR void VKAPI_CALL CmdSetDepthTestEnableEXT(
		VkCommandBuffer                             commandBuffer,
		VkBool32                                    depthTestEnable) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetDepthTestEnableCmd>(cb);
	cmd.enable = depthTestEnable;

	cb.dev->dispatch.CmdSetDepthTestEnableEXT(cb.handle(), depthTestEnable);
}

VKAPI_ATTR void VKAPI_CALL CmdSetDepthWriteEnableEXT(
		VkCommandBuffer                             commandBuffer,
		VkBool32                                    depthWriteEnable) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetDepthWriteEnableCmd>(cb);
	cmd.enable = depthWriteEnable;

	cb.dev->dispatch.CmdSetDepthWriteEnableEXT(cb.handle(), depthWriteEnable);
}

VKAPI_ATTR void VKAPI_CALL CmdSetDepthCompareOpEXT(
		VkCommandBuffer                             commandBuffer,
		VkCompareOp                                 depthCompareOp) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetDepthCompareOpCmd>(cb);
	cmd.op = depthCompareOp;

	cb.dev->dispatch.CmdSetDepthCompareOpEXT(cb.handle(), depthCompareOp);
}

VKAPI_ATTR void VKAPI_CALL CmdSetDepthBoundsTestEnableEXT(
		VkCommandBuffer                             commandBuffer,
		VkBool32                                    depthBoundsTestEnable) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetDepthBoundsTestEnableCmd>(cb);
	cmd.enable = depthBoundsTestEnable;

	cb.dev->dispatch.CmdSetDepthBoundsTestEnableEXT(cb.handle(), depthBoundsTestEnable);
}

VKAPI_ATTR void VKAPI_CALL CmdSetStencilTestEnableEXT(
		VkCommandBuffer                             commandBuffer,
		VkBool32                                    stencilTestEnable) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetStencilTestEnableCmd>(cb);
	cmd.enable = stencilTestEnable;

	cb.dev->dispatch.CmdSetStencilTestEnableEXT(cb.handle(), stencilTestEnable);
}

VKAPI_ATTR void VKAPI_CALL CmdSetStencilOpEXT(
		VkCommandBuffer                             commandBuffer,
		VkStencilFaceFlags                          faceMask,
		VkStencilOp                                 failOp,
		VkStencilOp                                 passOp,
		VkStencilOp                                 depthFailOp,
		VkCompareOp                                 compareOp) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetStencilOpCmd>(cb);
	cmd.faceMask = faceMask;
	cmd.failOp = failOp;
	cmd.passOp = passOp;
	cmd.depthFailOp = depthFailOp;
	cmd.compareOp = compareOp;

	cb.dev->dispatch.CmdSetStencilOpEXT(cb.handle(), faceMask,
		failOp, passOp, depthFailOp, compareOp);
}

VKAPI_ATTR void VKAPI_CALL CmdSetSampleLocationsEXT(
		VkCommandBuffer                             commandBuffer,
		const VkSampleLocationsInfoEXT*             pSampleLocationsInfo) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetSampleLocationsCmd>(cb);
	cmd.info = *pSampleLocationsInfo;
	copyChainInPlace(cb, cmd.info.pNext);

	cb.dev->dispatch.CmdSetSampleLocationsEXT(cb.handle(), pSampleLocationsInfo);
}

VKAPI_ATTR void VKAPI_CALL CmdSetDiscardRectangleEXT(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    firstDiscardRectangle,
		uint32_t                                    discardRectangleCount,
		const VkRect2D*                             pDiscardRectangles) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetDiscardRectangleCmd>(cb);
	cmd.first = firstDiscardRectangle;
	cmd.rects = copySpan(cb, pDiscardRectangles, discardRectangleCount);

	cb.dev->dispatch.CmdSetDiscardRectangleEXT(cb.handle(),
		firstDiscardRectangle, discardRectangleCount, pDiscardRectangles);
}

// VK_KHR_acceleration_structure
VKAPI_ATTR void VKAPI_CALL CmdBuildAccelerationStructuresKHR(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    infoCount,
		const VkAccelerationStructureBuildGeometryInfoKHR* pInfos,
		const VkAccelerationStructureBuildRangeInfoKHR* const* ppBuildRangeInfos) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<BuildAccelStructsCmd>(cb, cb);

	cmd.srcs = allocSpan0<AccelStruct*>(cb, infoCount);
	cmd.dsts = allocSpan0<AccelStruct*>(cb, infoCount);
	cmd.buildRangeInfos = allocSpan<span<VkAccelerationStructureBuildRangeInfoKHR>>(cb, infoCount);

	cmd.buildInfos = copySpan(cb, pInfos, infoCount);
	for(auto i = 0u; i < infoCount; ++i) {
		auto& buildInfo = cmd.buildInfos[i];
		copyChainInPlace(cb, buildInfo.pNext);

		// We have to perform a deep-copy of the geometry descriptions.
		if(buildInfo.pGeometries) {
			buildInfo.pGeometries = copySpan(cb, buildInfo.pGeometries, buildInfo.geometryCount).data();
		} else if(buildInfo.geometryCount > 0) {
			dlg_assert(buildInfo.ppGeometries);
			auto dst = allocSpan<VkAccelerationStructureGeometryKHR>(cb, buildInfo.geometryCount);
			for(auto g = 0u; g < buildInfo.geometryCount; ++g) {
				dst[g] = *buildInfo.ppGeometries[g];
			}

			buildInfo.pGeometries = dst.data();
		}

		if(buildInfo.srcAccelerationStructure) {
			cmd.srcs[i] = &get(*cb.dev, buildInfo.srcAccelerationStructure);
			buildInfo.srcAccelerationStructure = cmd.srcs[i]->handle;
			useHandle(cb, cmd, *cmd.srcs[i]);
		}

		cmd.dsts[i] = &get(*cb.dev, buildInfo.dstAccelerationStructure);
		buildInfo.dstAccelerationStructure = cmd.dsts[i]->handle;
		useHandle(cb, cmd, *cmd.dsts[i]);

		cmd.buildRangeInfos[i] = copySpan(cb, ppBuildRangeInfos[i], buildInfo.geometryCount);

		// TODO: useHandle for buffers of associated device addresses
	}

	cb.record()->buildsAccelStructs = true;

	cb.dev->dispatch.CmdBuildAccelerationStructuresKHR(cb.handle(),
		infoCount, cmd.buildInfos.data(), ppBuildRangeInfos);
}

VKAPI_ATTR void VKAPI_CALL CmdBuildAccelerationStructuresIndirectKHR(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    infoCount,
		const VkAccelerationStructureBuildGeometryInfoKHR* pInfos,
		const VkDeviceAddress*                      pIndirectDeviceAddresses,
		const uint32_t*                             pIndirectStrides,
		const uint32_t* const*                      ppMaxPrimitiveCounts) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<BuildAccelStructsIndirectCmd>(cb, cb);

	cmd.srcs = allocSpan0<AccelStruct*>(cb, infoCount);
	cmd.dsts = allocSpan0<AccelStruct*>(cb, infoCount);
	cmd.indirectAddresses = allocSpan<VkDeviceAddress>(cb, infoCount);
	cmd.indirectStrides = allocSpan<u32>(cb, infoCount);
	cmd.maxPrimitiveCounts = allocSpan<u32*>(cb, infoCount);

	cmd.buildInfos = copySpan(cb, pInfos, infoCount);
	for(auto i = 0u; i < infoCount; ++i) {
		auto& buildInfo = cmd.buildInfos[i];
		copyChainInPlace(cb, buildInfo.pNext);

		if(buildInfo.srcAccelerationStructure) {
			cmd.srcs[i] = &get(*cb.dev, buildInfo.srcAccelerationStructure);
			buildInfo.srcAccelerationStructure = cmd.srcs[i]->handle;
			useHandle(cb, cmd, *cmd.srcs[i]);
		}

		cmd.dsts[i] = &get(*cb.dev, buildInfo.dstAccelerationStructure);
		buildInfo.dstAccelerationStructure = cmd.dsts[i]->handle;
		useHandle(cb, cmd, *cmd.dsts[i]);

		cmd.indirectAddresses[i] = pIndirectDeviceAddresses[i];
		cmd.indirectStrides[i] = pIndirectStrides[i];
		cmd.maxPrimitiveCounts[i] = copySpan(cb,
			ppMaxPrimitiveCounts[i], buildInfo.geometryCount).data();

		// TODO: useHandle for buffers of associated device addresses?
	}

	cb.record()->buildsAccelStructs = true;

	cb.dev->dispatch.CmdBuildAccelerationStructuresIndirectKHR(cb.handle(),
		infoCount, cmd.buildInfos.data(), pIndirectDeviceAddresses,
		pIndirectStrides, ppMaxPrimitiveCounts);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyAccelerationStructureKHR(
		VkCommandBuffer                             commandBuffer,
		const VkCopyAccelerationStructureInfoKHR*   pInfo) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<CopyAccelStructureCmd>(cb);
	cmd.pNext = copyChain(cb, pInfo->pNext);
	cmd.src = &get(*cb.dev, pInfo->src);
	cmd.dst = &get(*cb.dev, pInfo->dst);
	cmd.mode = pInfo->mode;

	useHandle(cb, cmd, *cmd.src);
	useHandle(cb, cmd, *cmd.dst);

	auto fwd = *pInfo;
	fwd.src = cmd.src->handle;
	fwd.dst = cmd.src->handle;

	cb.dev->dispatch.CmdCopyAccelerationStructureKHR(cb.handle(), &fwd);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyAccelerationStructureToMemoryKHR(
		VkCommandBuffer                             commandBuffer,
		const VkCopyAccelerationStructureToMemoryInfoKHR* pInfo) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<CopyAccelStructToMemoryCmd>(cb);
	cmd.pNext = copyChain(cb, pInfo->pNext);
	cmd.src = &get(*cb.dev, pInfo->src);
	cmd.dst = pInfo->dst;
	cmd.mode = pInfo->mode;

	useHandle(cb, cmd, *cmd.src);
	// TODO: useHandle for buffers of associated device addresses?

	auto fwd = *pInfo;
	fwd.src = cmd.src->handle;
	cb.dev->dispatch.CmdCopyAccelerationStructureToMemoryKHR(cb.handle(), &fwd);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyMemoryToAccelerationStructureKHR(
		VkCommandBuffer                             commandBuffer,
		const VkCopyMemoryToAccelerationStructureInfoKHR* pInfo) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<CopyMemoryToAccelStructCmd>(cb);
	cmd.pNext = copyChain(cb, pInfo->pNext);
	cmd.src = pInfo->src;
	cmd.dst = &get(*cb.dev, pInfo->dst);
	cmd.mode = pInfo->mode;

	useHandle(cb, cmd, *cmd.dst);
	// TODO: useHandle for buffers of associated device addresses?

	auto fwd = *pInfo;
	fwd.dst = cmd.dst->handle;
	cb.dev->dispatch.CmdCopyMemoryToAccelerationStructureKHR(cb.handle(), &fwd);
}

VKAPI_ATTR void VKAPI_CALL CmdWriteAccelerationStructuresPropertiesKHR(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    accelerationStructureCount,
		const VkAccelerationStructureKHR*           pAccelerationStructures,
		VkQueryType                                 queryType,
		VkQueryPool                                 queryPool,
		uint32_t                                    firstQuery) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<WriteAccelStructsPropertiesCmd>(cb);
	cmd.firstQuery = firstQuery;
	cmd.queryType = queryType;
	cmd.queryPool = &get(*cb.dev, queryPool);
	useHandle(cb, cmd, *cmd.queryPool);

	cmd.accelStructs = allocSpan<AccelStruct*>(cb, accelerationStructureCount);

	ThreadMemScope memScope;
	auto fwd = memScope.alloc<VkAccelerationStructureKHR>(accelerationStructureCount);

	for(auto i = 0u; i < accelerationStructureCount; ++i) {
		cmd.accelStructs[i] = &get(*cb.dev, pAccelerationStructures[i]);
		useHandle(cb, cmd, *cmd.accelStructs[i]);
		fwd[i] = cmd.accelStructs[i]->handle;
	}

	cb.dev->dispatch.CmdWriteAccelerationStructuresPropertiesKHR(cb.handle(),
		fwd.size(), fwd.data(), queryType, cmd.queryPool->handle, firstQuery);
}


// VK_KHR_ray_tracing_pipeline
VKAPI_ATTR void VKAPI_CALL CmdTraceRaysKHR(
		VkCommandBuffer                             commandBuffer,
		const VkStridedDeviceAddressRegionKHR*      pRaygenShaderBindingTable,
		const VkStridedDeviceAddressRegionKHR*      pMissShaderBindingTable,
		const VkStridedDeviceAddressRegionKHR*      pHitShaderBindingTable,
		const VkStridedDeviceAddressRegionKHR*      pCallableShaderBindingTable,
		uint32_t                                    width,
		uint32_t                                    height,
		uint32_t                                    depth) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<TraceRaysCmd>(cb, cb, cb.rayTracingState());
	cmd.raygenBindingTable = *pRaygenShaderBindingTable;
	cmd.missBindingTable = *pMissShaderBindingTable;
	cmd.hitBindingTable = *pHitShaderBindingTable;
	cmd.callableBindingTable = *pCallableShaderBindingTable;
	cmd.width = width;
	cmd.height = height;
	cmd.depth = depth;

	// TODO: useHandle for buffers of associated device addresses?

	cb.dev->dispatch.CmdTraceRaysKHR(cb.handle(),
		pRaygenShaderBindingTable,
		pMissShaderBindingTable,
		pHitShaderBindingTable,
		pCallableShaderBindingTable,
		width, height, depth);
}

VKAPI_ATTR void VKAPI_CALL CmdTraceRaysIndirectKHR(
		VkCommandBuffer                             commandBuffer,
		const VkStridedDeviceAddressRegionKHR*      pRaygenShaderBindingTable,
		const VkStridedDeviceAddressRegionKHR*      pMissShaderBindingTable,
		const VkStridedDeviceAddressRegionKHR*      pHitShaderBindingTable,
		const VkStridedDeviceAddressRegionKHR*      pCallableShaderBindingTable,
		VkDeviceAddress                             indirectDeviceAddress) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<TraceRaysIndirectCmd>(cb, cb, cb.rayTracingState());
	cmd.raygenBindingTable = *pRaygenShaderBindingTable;
	cmd.missBindingTable = *pMissShaderBindingTable;
	cmd.hitBindingTable = *pHitShaderBindingTable;
	cmd.callableBindingTable = *pCallableShaderBindingTable;
	cmd.indirectDeviceAddress = indirectDeviceAddress;

	// TODO: useHandle for buffers of associated device addresses?

	cb.dev->dispatch.CmdTraceRaysIndirectKHR(cb.handle(),
		pRaygenShaderBindingTable,
		pMissShaderBindingTable,
		pHitShaderBindingTable,
		pCallableShaderBindingTable,
		indirectDeviceAddress);
}

VKAPI_ATTR void VKAPI_CALL CmdSetRayTracingPipelineStackSizeKHR(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    pipelineStackSize) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetRayTracingPipelineStackSizeCmd>(cb);
	cmd.stackSize = pipelineStackSize;

	cb.dev->dispatch.CmdSetRayTracingPipelineStackSizeKHR(cb.handle(), pipelineStackSize);
}

} // namespace vil
