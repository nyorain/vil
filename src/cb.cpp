#include <cb.hpp>
#include <data.hpp>
#include <ds.hpp>
#include <queue.hpp>
#include <handles.hpp>
#include <commands.hpp>
#include <record.hpp>
#include <gui/commandHook.hpp>
#include <util/util.hpp>
#include <util/ext.hpp>

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
		}
	}

	// CommandBuffer is dispatchable handle, we need to remove this association
	eraseData(handle_);

	// Remove ourselves from the pool we come from.
	// A command pool can't be destroyed before its command buffers (it
	// implicitly frees them).
	auto it = find(pool_->cbs, this);
	dlg_assert(it != pool_->cbs.end());
	pool_->cbs.erase(it);
}

void CommandBuffer::clearPendingLocked() {
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

void CommandBuffer::doReset(bool record) {
	// Make sure to never destroy a CommandBufferRecord inside the
	// device lock.
	IntrusivePtr<CommandRecord> keepAliveRecord;

	{
		std::lock_guard lock(dev->mutex);
		clearPendingLocked();

		if(record_) {
			record_->cb = nullptr;
			keepAliveRecord = std::move(record_);
		}

		// We have to lock our own mutex since other threads might read
		// our data at the same time.
		if(record) {
			++recordCount_;
			// TODO: with some custom IntrusivePtr deleter we could
			// allocate the CommandBufferRecord inside its own mem block
			// (that we first allocate here, then create the record in it,
			// them move the block into the record).
			record_ = IntrusivePtr<CommandRecord>(new CommandRecord(*this));

			// TODO: should this be in CommandRecord constructor?
			auto& deleter = record_->memBlocks.get_deleter();
			pool_->records.insert(&deleter);

			state_ = CommandBuffer::State::recording;
		} else {
			state_ = CommandBuffer::State::initial;
		}
	}
}

void CommandBuffer::doEnd() {
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
	pushConstants_ = {};

	section_ = nullptr;
	lastCommand_ = nullptr;
	ignoreEndDebugLabels_ = 0u;

	// parse commands into description
	// record_->desc = CommandBufferDesc::get(record_->commands);
	record_->desc = CommandBufferDesc::getAnnotate(record_->commands);

	// Make sure to never call CommandBufferRecord destructor inside lock
	auto keepAliveRecord = lastRecord_;

	// Critical section
	{
		std::lock_guard lock(dev->mutex);

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
	while(dynamic_cast<BeginDebugUtilsLabelCmd*>(section_->cmd)) {
		dlg_assert(dynamic_cast<BeginDebugUtilsLabelCmd*>(section_->cmd));
		dlg_trace("Problematic debug utils label nesting detected "
			"(Begin without end in scope)");
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
	record_->hook.reset();
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

	{
		std::lock_guard lock(dev->mutex);
		for(auto& recordDeleter : records) {
			recordDeleter->pool = nullptr;
		}
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
		dev->commandBuffers.mustErase(cb->handle());
	}

	freeBlocks(memBlocks.load());
}

template<typename T>
void ensureSize(CommandBuffer& cb, span<T>& buf, std::size_t size) {
	if(buf.size() >= size) {
		return;
	}

	auto newBuf = allocSpan<T>(cb, size);
	std::copy(buf.begin(), buf.end(), newBuf.begin());
	buf = newBuf;
}

template<typename D, typename T>
void upgradeSpan(CommandBuffer& cb, span<D>& dst, T* data, std::size_t count) {
	dst = allocSpan<D>(cb, count);
	for(auto i = 0u; i < count; ++i) {
		dst[i] = upgrade(data[i]);
	}
}

// util
void DescriptorState::bind(CommandBuffer& cb, PipelineLayout& layout, u32 firstSet,
		span<DescriptorSet* const> sets, span<const u32>) {
	ensureSize(cb, descriptorSets, firstSet + sets.size());

	// NOTE: the "ds disturbing" part of vulkan is hard to grasp IMO.
	// There may be errors here.
	// TODO: do we even need to track it like this? only useful if we
	// also show it in UI which sets were disturbed.

	auto lastSet = firstSet + sets.size() - 1;
	for(auto i = 0u; i < firstSet; ++i) {
		if(!descriptorSets[i].ds) {
			continue;
		}

		dlg_assert(descriptorSets[i].layout);
		if(!compatibleForSetN(*descriptorSets[i].layout, layout, i)) {
			// disturbed!
			// dlg_debug("disturbed ds {}", i);
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
		// dlg_debug("disturbed following descriptorSets, from {}", lastSet + 1);
		descriptorSets = descriptorSets.subspan(0, lastSet + 1);
	}
}

void copy(CommandBuffer& cb, const DescriptorState& src, DescriptorState& dst) {
	dst.descriptorSets = copySpan(cb, src.descriptorSets);
}

GraphicsState copy(CommandBuffer& cb, const GraphicsState& src) {
	GraphicsState dst = src;
	copy(cb, src, dst); // descriptors

	dst.vertices = copySpan(cb, src.vertices);
	dst.dynamic.viewports = copySpan(cb, src.dynamic.viewports);
	dst.dynamic.scissors = copySpan(cb, src.dynamic.scissors);

	return dst;
}

ComputeState copy(CommandBuffer& cb, const ComputeState& src) {
	ComputeState dst = src;
	copy(cb, src, dst); // descriptors
	return dst;
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
		cb->doReset(false);
	}
	return dev.dispatch.ResetCommandPool(device, commandPool, flags);
}

VKAPI_ATTR void VKAPI_CALL TrimCommandPool(
		VkDevice                                    device,
		VkCommandPool                               commandPool,
		VkCommandPoolTrimFlags                      flags) {
	auto& dev = getData<Device>(device);
	auto& pool = dev.commandPools.get(commandPool);

	// free all currently unused memory blocks
	auto* blocks = pool.memBlocks.load();
	while(!pool.memBlocks.compare_exchange_weak(blocks, nullptr));
	freeBlocks(blocks);

	auto f = dev.dispatch.TrimCommandPool;
	f(device, commandPool, flags);
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

	auto& cp = dev.commandPools.get(pAllocateInfo->commandPool);
	for(auto i = 0u; i < pAllocateInfo->commandBufferCount; ++i) {
		auto& cb = dev.commandBuffers.add(pCommandBuffers[i], cp, pCommandBuffers[i]);
		cb.dev = &dev;
		cb.objectType = VK_OBJECT_TYPE_COMMAND_BUFFER;
		cb.pool().cbs.push_back(&cb);

		// command buffers are dispatchable, add global data entry
		vil::insertData(pCommandBuffers[i], &cb);
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
		dev.commandBuffers.mustErase(pCommandBuffers[i]);
	}

	dev.dispatch.FreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(
		VkCommandBuffer                             commandBuffer,
		const VkCommandBufferBeginInfo*             pBeginInfo) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	cb.doReset(true);
	cb.record()->usageFlags = pBeginInfo->flags;

	if(pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) {
		dlg_assert(pBeginInfo->pInheritanceInfo);
		dlg_assert(pBeginInfo->pInheritanceInfo->framebuffer);
		dlg_assert(pBeginInfo->pInheritanceInfo->renderPass);

		auto& fb = cb.dev->framebuffers.get(pBeginInfo->pInheritanceInfo->framebuffer);
		auto& rp = cb.dev->renderPasses.get(pBeginInfo->pInheritanceInfo->renderPass);

		cb.graphicsState().rpi.fb = &fb;
		cb.graphicsState().rpi.rp = &rp;
		cb.graphicsState().rpi.subpass = pBeginInfo->pInheritanceInfo->subpass;

		// TODO: use handles here? would have to allow using handles without command
		// Pretty sure we'd have to do it for correctness.
		// useHandle(cb, cmd, *cmd.fb);
		// useHandle(cb, cmd, *cmd.rp);
	}

	return cb.dev->dispatch.BeginCommandBuffer(commandBuffer, pBeginInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL EndCommandBuffer(
		VkCommandBuffer                             commandBuffer) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	cb.doEnd();
	return cb.dev->dispatch.EndCommandBuffer(commandBuffer);
}

VKAPI_ATTR VkResult VKAPI_CALL ResetCommandBuffer(
		VkCommandBuffer                             commandBuffer,
		VkCommandBufferResetFlags                   flags) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	cb.doReset(false);
	return cb.dev->dispatch.ResetCommandBuffer(commandBuffer, flags);
}

// == command buffer recording ==
// util
// void addToHandle(CommandBufferRecord& rec, DeviceHandle& handle) {
// 	std::lock_guard lock(rec.device().mutex);
// 	auto [it, success] = handle.refCbs.insert(&rec);
// 	dlg_assert(success);
// }

void useHandle(CommandRecord& rec, Command& cmd, u64 h64, DeviceHandle& handle) {
	// auto it = rec.handles.find(h64);
	// if(it == rec.handles.end()) {
	// 	it = rec.handles.emplace(h64, UsedHandle{handle, *rec.cb}).first;
	// 	it->second.handle = &handle;
	// 	addToHandle(rec, *it->second.handle);
	// }

	auto it = rec.handles.emplace(h64, UsedHandle{handle, rec}).first;
	it->second.commands.push_back(&cmd);
}

template<typename T>
void useHandle(CommandRecord& rec, Command& cmd, T& handle) {
	auto h64 = handleToU64(vil::handle(handle));
	useHandle(rec, cmd, h64, handle);
}

UsedImage& useHandle(CommandRecord& rec, Command& cmd, Image& image) {
	// auto it = rec.images.find(image.handle);
	// if(it == rec.images.end()) {
	// 	it = rec.images.emplace(image.handle, UsedImage(image, *rec.cb)).first;
	// 	it->second.image = &image;
	// 	addToHandle(rec, *it->second.image);
	// }

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
	cmd.memBarriers = copySpan(cb, pMemoryBarriers, memoryBarrierCount);
	cmd.imgBarriers = copySpan(cb, pImageMemoryBarriers, imageMemoryBarrierCount);
	cmd.bufBarriers = copySpan(cb, pBufferMemoryBarriers, bufferMemoryBarrierCount);

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
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<WaitEventsCmd>(cb);
	cmdBarrier(cb, cmd, srcStageMask, dstStageMask,
		memoryBarrierCount, pMemoryBarriers,
		bufferMemoryBarrierCount, pBufferMemoryBarriers,
		imageMemoryBarrierCount, pImageMemoryBarriers);

	cmd.events = allocSpan<Event*>(cb, eventCount);
	for(auto i = 0u; i < eventCount; ++i) {
		auto& event = cb.dev->events.get(pEvents[i]);
		cmd.events[i] = &event;
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

void cmdBeginRenderPass(CommandBuffer& cb,
		const VkRenderPassBeginInfo& rpBeginInfo,
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
	cb.graphicsState().rpi.fb = cmd.fb;
	cb.graphicsState().rpi.subpass = 0u;

	cmd.subpassBeginInfo = subpassBeginInfo;
	copyChainInPlace(cb, cmd.subpassBeginInfo.pNext);

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

			useHandle(cb, cmd, *attachment, false);
			useHandle(cb, cmd, *attachment->img,
				cmd.rp->desc->attachments[i].finalLayout);
		}
	}

	auto& subpassCmd = addCmd<FirstSubpassCmd, SectionType::begin>(cb);
	(void) subpassCmd;
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(
		VkCommandBuffer                             commandBuffer,
		const VkRenderPassBeginInfo*                pRenderPassBegin,
		VkSubpassContents                           contents) {
	auto& cb = getData<CommandBuffer>(commandBuffer);

	VkSubpassBeginInfo subpassBeginInfo = {};
	subpassBeginInfo.contents = contents;
	subpassBeginInfo.sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO;

	cmdBeginRenderPass(cb, *pRenderPassBegin, subpassBeginInfo);
	cb.dev->dispatch.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
}

VKAPI_ATTR void VKAPI_CALL CmdNextSubpass(
		VkCommandBuffer                             commandBuffer,
		VkSubpassContents                           contents) {
	auto& cb = getData<CommandBuffer>(commandBuffer);

	cb.popLabelSections();
	dlg_assert(cb.section() && dynamic_cast<SubpassCmd*>(cb.section()->cmd));

	auto& cmd = addCmd<NextSubpassCmd, SectionType::next>(cb);
	cmd.beginInfo = {};
	cmd.beginInfo.sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO;
	cmd.beginInfo.contents = contents;

	dlg_assert(cb.graphicsState().rpi.rp);
	++cb.graphicsState().rpi.subpass;

	cb.dev->dispatch.CmdNextSubpass(commandBuffer, contents);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(
		VkCommandBuffer                             commandBuffer) {
	auto& cb = getData<CommandBuffer>(commandBuffer);

	cb.popLabelSections();
	dlg_assert(cb.section() && dynamic_cast<SubpassCmd*>(cb.section()->cmd));

	cb.endSection(nullptr); // pop subpass section
	dlg_assert(cb.section() && dynamic_cast<BeginRenderPassCmd*>(cb.section()->cmd));

	auto& cmd = addCmd<EndRenderPassCmd, SectionType::end>(cb);
	cmd.endInfo = {};
	cmd.endInfo.sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO;

	dlg_assert(cb.graphicsState().rpi.rp);
	cb.graphicsState().rpi = {};

	cb.dev->dispatch.CmdEndRenderPass(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass2(
		VkCommandBuffer                             commandBuffer,
		const VkRenderPassBeginInfo*                pRenderPassBegin,
		const VkSubpassBeginInfo*                   pSubpassBeginInfo) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	cmdBeginRenderPass(cb, *pRenderPassBegin, *pSubpassBeginInfo);

	auto f = cb.dev->dispatch.CmdBeginRenderPass2;
	f(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
}

VKAPI_ATTR void VKAPI_CALL CmdNextSubpass2(
		VkCommandBuffer                             commandBuffer,
		const VkSubpassBeginInfo*                   pSubpassBeginInfo,
		const VkSubpassEndInfo*                     pSubpassEndInfo) {
	auto& cb = getData<CommandBuffer>(commandBuffer);

	cb.popLabelSections();
	dlg_assert(cb.section() && dynamic_cast<SubpassCmd*>(cb.section()->cmd));

	auto& cmd = addCmd<NextSubpassCmd, SectionType::next>(cb);
	cmd.beginInfo = *pSubpassBeginInfo;
	copyChainInPlace(cb, cmd.beginInfo.pNext);

	dlg_assert(cb.graphicsState().rpi.rp);
	++cb.graphicsState().rpi.subpass;

	auto f = cb.dev->dispatch.CmdNextSubpass2;
	f(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass2(
		VkCommandBuffer                             commandBuffer,
		const VkSubpassEndInfo*                     pSubpassEndInfo) {
	auto& cb = getData<CommandBuffer>(commandBuffer);

	cb.popLabelSections();
	dlg_assert(cb.section() && dynamic_cast<SubpassCmd*>(cb.section()->cmd));

	cb.endSection(nullptr); // pop subpass section
	dlg_assert(cb.section() && dynamic_cast<BeginRenderPassCmd*>(cb.section()->cmd));

	auto& cmd = addCmd<EndRenderPassCmd, SectionType::end>(cb);
	cmd.endInfo = *pSubpassEndInfo;
	copyChainInPlace(cb, cmd.endInfo.pNext);

	dlg_assert(cb.graphicsState().rpi.rp);
	cb.graphicsState().rpi = {};

	auto f = cb.dev->dispatch.CmdEndRenderPass2;
	f(commandBuffer, pSubpassEndInfo);
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
	cmd.dynamicOffsets = copySpan(cb, pDynamicOffsets, dynamicOffsetCount);

	// NOTE: the pipeline layout is intetionally not added to used handles
	// since the application not destroying it does not move the command
	// buffer into invalid state (and vulkan requires that it's kept
	// alive while recording).
	// Since we might need it later on, we acquire shared ownership.
	// Also like this in CmdPushConstants
	auto pipeLayoutPtr = cb.dev->pipeLayouts.getPtr(layout);
	cmd.pipeLayout = pipeLayoutPtr.get();
	cb.record()->pipeLayouts.emplace_back(std::move(pipeLayoutPtr));

	cmd.sets = allocSpan<DescriptorSet*>(cb, descriptorSetCount);
	for(auto i = 0u; i < descriptorSetCount; ++i) {
		auto& ds = cb.dev->descriptorSets.get(pDescriptorSets[i]);

		useHandle(cb, cmd, ds);
		cmd.sets[i] = &ds;
	}

	// TODO: not sure about this. The spec isn't clear about this.
	// But this seems to be what the validation layers do.
	// https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/57f6f2a387b37c442c4db6993eb064a1e750b30f/layers/state_tracker.cpp#L5868
	// if(cb.pushConstants.layout &&
	// 		!pushConstantCompatible(*cmd.pipeLayout, *cb.pushConstants.layout)) {
	// 	cb.pushConstants.layout = nullptr;
	// 	cb.pushConstants.map.clear();
	// }

	// update bound state
	if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
		cb.computeState().bind(cb, *cmd.pipeLayout, firstSet, cmd.sets,
			{pDynamicOffsets, pDynamicOffsets + dynamicOffsetCount});
	} else if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
		cb.graphicsState().bind(cb, *cmd.pipeLayout, firstSet, cmd.sets,
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

	cb.graphicsState().indices.buffer = &buf;
	cb.graphicsState().indices.offset = offset;
	cb.graphicsState().indices.type = indexType;

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

	ensureSize(cb, cb.graphicsState().vertices, firstBinding + bindingCount);
	cmd.buffers = allocSpan<BoundVertexBuffer>(cb, bindingCount);
	for(auto i = 0u; i < bindingCount; ++i) {
		auto& buf = cb.dev->buffers.get(pBuffers[i]);
		cmd.buffers[i].buffer = &buf;
		cmd.buffers[i].offset = pOffsets[i];
		useHandle(cb, cmd, buf);

		cb.graphicsState().vertices[firstBinding + i].buffer = &buf;
		cb.graphicsState().vertices[firstBinding + i].offset = pOffsets[i];
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
	auto& cmd = addCmd<DrawCmd>(cb, cb, cb.graphicsState());

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
	auto& cmd = addCmd<DrawIndexedCmd>(cb, cb, cb.graphicsState());

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
	auto& cmd = addCmd<DrawIndirectCmd>(cb, cb, cb.graphicsState());

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
	auto& cmd = addCmd<DrawIndirectCmd>(cb, cb, cb.graphicsState());

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
	auto& cmd = addCmd<DrawIndirectCountCmd>(cb, cb, cb.graphicsState());

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

	auto f = cb.dev->dispatch.CmdDrawIndirectCount;
	f(commandBuffer, buffer, offset, countBuffer, countBufferOffset,
		maxDrawCount, stride);
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
	auto& cmd = addCmd<DrawIndirectCountCmd>(cb, cb, cb.graphicsState());

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

	auto f = cb.dev->dispatch.CmdDrawIndexedIndirectCount;
	f(commandBuffer, buffer, offset, countBuffer, countBufferOffset,
		maxDrawCount, stride);
}

VKAPI_ATTR void VKAPI_CALL CmdDispatch(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    groupCountX,
		uint32_t                                    groupCountY,
		uint32_t                                    groupCountZ) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<DispatchCmd>(cb, cb, cb.computeState());

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
	auto& cmd = addCmd<DispatchIndirectCmd>(cb, cb, cb.computeState());
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
	auto& cmd = addCmd<DispatchBaseCmd>(cb, cb, cb.computeState());

	cmd.baseGroupX = baseGroupX;
	cmd.baseGroupY = baseGroupY;
	cmd.baseGroupZ = baseGroupZ;
	cmd.groupsX = groupCountX;
	cmd.groupsY = groupCountY;
	cmd.groupsZ = groupCountZ;

	auto f = cb.dev->dispatch.CmdDispatchBase;
	f(commandBuffer, baseGroupX, baseGroupY, baseGroupZ,
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
	upgradeSpan(cb, cmd.copies, pRegions, regionCount);

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

	cb.dev->dispatch.CmdCopyImage(commandBuffer,
		srcImage, srcImageLayout,
		dstImage, dstImageLayout,
		regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2KHR(
		VkCommandBuffer                             commandBuffer,
		const VkCopyImageInfo2KHR*                  info) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
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

	cb.dev->dispatch.CmdCopyImage2KHR(commandBuffer, info);
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
	upgradeSpan(cb, cmd.blits, pRegions, regionCount);
	cmd.filter = filter;

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

	cb.dev->dispatch.CmdBlitImage(commandBuffer,
		srcImage, srcImageLayout,
		dstImage, dstImageLayout,
		regionCount, pRegions, filter);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2KHR(
		VkCommandBuffer                             commandBuffer,
		const VkBlitImageInfo2KHR*                  info) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
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

	cb.dev->dispatch.CmdBlitImage2KHR(commandBuffer, info);
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
	cmd.dstLayout = dstImageLayout;
	upgradeSpan(cb, cmd.copies, pRegions, regionCount);

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

	cb.dev->dispatch.CmdCopyBufferToImage(commandBuffer,
		srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage2KHR(
		VkCommandBuffer                             commandBuffer,
		const VkCopyBufferToImageInfo2KHR*          info) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
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

	cb.dev->dispatch.CmdCopyBufferToImage2KHR(commandBuffer, info);
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
	cmd.srcLayout = srcImageLayout;
	upgradeSpan(cb, cmd.copies, pRegions, regionCount);

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

	cb.dev->dispatch.CmdCopyImageToBuffer(commandBuffer,
		srcImage, srcImageLayout, dstBuffer, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer2KHR(
		VkCommandBuffer                             commandBuffer,
		const VkCopyImageToBufferInfo2KHR*          info) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
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

	cb.dev->dispatch.CmdCopyImageToBuffer2KHR(commandBuffer, info);
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
	cmd.dstLayout = imageLayout;
	cmd.ranges = copySpan(cb, pRanges, rangeCount);

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
	cmd.dstLayout = imageLayout;
	cmd.value = *pDepthStencil;
	cmd.ranges = copySpan(cb, pRanges, rangeCount);

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

	cmd.attachments = copySpan(cb, pAttachments, attachmentCount);
	cmd.rects = copySpan(cb, pRects, rectCount);

	dlg_assert(cb.graphicsState().rpi.rp);
	cmd.rpi = cb.graphicsState().rpi;

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
	upgradeSpan(cb, cmd.regions, pRegions, regionCount);

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

	cb.dev->dispatch.CmdResolveImage(commandBuffer, srcImage, srcImageLayout,
		dstImage, dstImageLayout, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage2KHR(
		VkCommandBuffer                             commandBuffer,
		const VkResolveImageInfo2KHR*               info) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
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

	cb.dev->dispatch.CmdResolveImage2KHR(commandBuffer, info);
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
	auto* last = cmd.children_;

	for(auto i = 0u; i < commandBufferCount; ++i) {
		auto& secondary = cb.dev->commandBuffers.get(pCommandBuffers[i]);
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
	upgradeSpan(cb, cmd.regions, pRegions, regionCount);

	useHandle(cb, cmd, srcBuf);
	useHandle(cb, cmd, dstBuf);

	cb.dev->dispatch.CmdCopyBuffer(commandBuffer,
		srcBuffer, dstBuffer, regionCount, pRegions);
}


VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer2KHR(
		VkCommandBuffer                             commandBuffer,
		const VkCopyBufferInfo2KHR*                 info) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<CopyBufferCmd>(cb);

	auto& srcBuf = cb.dev->buffers.get(info->srcBuffer);
	auto& dstBuf = cb.dev->buffers.get(info->dstBuffer);

	cmd.pNext = copyChain(cb, info->pNext);
	cmd.src = &srcBuf;
	cmd.dst = &dstBuf;
	cmd.regions = copySpan(cb, info->pRegions, info->regionCount);

	useHandle(cb, cmd, srcBuf);
	useHandle(cb, cmd, dstBuf);

	cb.dev->dispatch.CmdCopyBuffer2KHR(commandBuffer, info);
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
	cmd.data = copySpan(cb, static_cast<const std::byte*>(dataPtr), dataSize);
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
	auto& cmd = addCmd<BeginDebugUtilsLabelCmd, SectionType::begin>(cb);

	auto* c = pLabelInfo->color;
	cmd.color = {c[0], c[1], c[2], c[3]};
	cmd.name = copyString(cb, pLabelInfo->pLabelName);
	// TODO: copy pNext?

	if(cb.dev->dispatch.CmdBeginDebugUtilsLabelEXT) {
		cb.dev->dispatch.CmdBeginDebugUtilsLabelEXT(commandBuffer, pLabelInfo);
	}
}

VKAPI_ATTR void VKAPI_CALL CmdEndDebugUtilsLabelEXT(
		VkCommandBuffer                             commandBuffer) {
	auto& cb = getData<CommandBuffer>(commandBuffer);

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
	addCmd<EndDebugUtilsLabelCmd/*, SectionType::end*/>(cb);

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
		cb.computeState().pipe = &cb.dev->computePipes.get(pipeline);
		cmd.pipe = cb.computeState().pipe;
	} else if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
		cb.graphicsState().pipe = &cb.dev->graphicsPipes.get(pipeline);
		cmd.pipe = cb.graphicsState().pipe;
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

	// NOTE: See BindDescriptorSets for rationale on pipe layout handling here.
	auto pipeLayoutPtr = cb.dev->pipeLayouts.getPtr(pipeLayout);
	cmd.pipeLayout = pipeLayoutPtr.get();
	cb.record()->pipeLayouts.emplace_back(std::move(pipeLayoutPtr));

	cmd.stages = stageFlags;
	cmd.offset = offset;
	auto ptr = static_cast<const std::byte*>(pValues);
	cmd.values = copySpan(cb, static_cast<const std::byte*>(ptr), size);

	ensureSize(cb, cb.pushConstants().data, std::max(offset + size, 128u));
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
	cmd.viewports = copySpan(cb, pViewports, viewportCount);

	ensureSize(cb, cb.graphicsState().dynamic.viewports, firstViewport + viewportCount);
	std::copy(pViewports, pViewports + viewportCount,
		cb.graphicsState().dynamic.viewports.begin() + firstViewport);

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
	cmd.scissors = copySpan(cb, pScissors, scissorCount);

	ensureSize(cb, cb.graphicsState().dynamic.scissors, firstScissor + scissorCount);
	std::copy(pScissors, pScissors + scissorCount,
		cb.graphicsState().dynamic.scissors.begin() + firstScissor);

	cb.dev->dispatch.CmdSetScissor(commandBuffer, firstScissor, scissorCount, pScissors);
}

VKAPI_ATTR void VKAPI_CALL CmdSetLineWidth(
		VkCommandBuffer                             commandBuffer,
		float                                       lineWidth) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<SetLineWidthCmd>(cb);
	cmd.width = lineWidth;

	cb.graphicsState().dynamic.lineWidth = lineWidth;
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
	cb.graphicsState().dynamic.depthBias = cmd.state;

	cb.dev->dispatch.CmdSetDepthBias(commandBuffer,
		depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
}

VKAPI_ATTR void VKAPI_CALL CmdSetBlendConstants(
		VkCommandBuffer                             commandBuffer,
		const float                                 blendConstants[4]) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<SetBlendConstantsCmd>(cb);
	std::memcpy(cmd.values.data(), blendConstants, sizeof(cmd.values));
	std::memcpy(cb.graphicsState().dynamic.blendConstants.data(), blendConstants,
		sizeof(cb.graphicsState().dynamic.blendConstants));

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
	cb.graphicsState().dynamic.depthBoundsMin = minDepthBounds;
	cb.graphicsState().dynamic.depthBoundsMax = maxDepthBounds;

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
		cb.graphicsState().dynamic.stencilFront.compareMask = compareMask;
	}
	if(faceMask & VK_STENCIL_FACE_BACK_BIT) {
		cb.graphicsState().dynamic.stencilBack.compareMask = compareMask;
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
		cb.graphicsState().dynamic.stencilFront.compareMask = writeMask;
	}
	if(faceMask & VK_STENCIL_FACE_BACK_BIT) {
		cb.graphicsState().dynamic.stencilBack.compareMask = writeMask;
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
		cb.graphicsState().dynamic.stencilFront.reference = reference;
	}
	if(faceMask & VK_STENCIL_FACE_BACK_BIT) {
		cb.graphicsState().dynamic.stencilBack.reference = reference;
	}

	cb.dev->dispatch.CmdSetStencilReference(commandBuffer, faceMask, reference);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetKHR(
		VkCommandBuffer                             commandBuffer,
		VkPipelineBindPoint                         pipelineBindPoint,
		VkPipelineLayout                            layout,
		uint32_t                                    set,
		uint32_t                                    descriptorWriteCount,
		const VkWriteDescriptorSet*                 pDescriptorWrites) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<PushDescriptorSetCmd>(cb);
	cmd.bindPoint = pipelineBindPoint;
	cmd.set = set;
	cmd.descriptorWrites = copySpan(cb, pDescriptorWrites, descriptorWriteCount);

	// deep-copy cmd.descriptorWrites.
	// The allocation done by copySpan is bound to the lifetime of the record
	// and we don't need to store the span itself anywhere since we don't
	// directly have ownership
	for(auto& write : cmd.descriptorWrites) {
		if(!write.descriptorCount) {
			continue;
		}

		switch(category(write.descriptorType)) {
			case DescriptorCategory::buffer:
				dlg_assert(write.pBufferInfo);
				write.pBufferInfo = copySpan(cb, write.pBufferInfo, write.descriptorCount).data();
				break;
			case DescriptorCategory::image:
				dlg_assert(write.pImageInfo);
				write.pImageInfo = copySpan(cb, write.pImageInfo, write.descriptorCount).data();
				break;
			case DescriptorCategory::bufferView:
				dlg_assert(write.pTexelBufferView);
				write.pTexelBufferView = copySpan(cb, write.pTexelBufferView, write.descriptorCount).data();
				break;
			default:
				dlg_error("Invalid/unknown descriptor type");
				break;
		}
	}

	auto pipeLayoutPtr = cb.dev->pipeLayouts.getPtr(layout);
	cmd.pipeLayout = pipeLayoutPtr.get();
	cb.record()->pipeLayouts.emplace_back(std::move(pipeLayoutPtr));

	cb.dev->dispatch.CmdPushDescriptorSetKHR(commandBuffer,
		pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetWithTemplateKHR(
		VkCommandBuffer                             commandBuffer,
		VkDescriptorUpdateTemplate                  descriptorUpdateTemplate,
		VkPipelineLayout                            layout,
		uint32_t                                    set,
		const void*                                 pData) {
	auto& cb = getData<CommandBuffer>(commandBuffer);
	auto& cmd = addCmd<PushDescriptorSetWithTemplateCmd>(cb);
	cmd.set = set;

	auto pipeLayoutPtr = cb.dev->pipeLayouts.getPtr(layout);
	cmd.pipeLayout = pipeLayoutPtr.get();
	cb.record()->pipeLayouts.emplace_back(std::move(pipeLayoutPtr));

	auto dsUpdateTemplate = cb.dev->dsuTemplates.getPtr(descriptorUpdateTemplate);
	cmd.updateTemplate = dsUpdateTemplate.get();
	cb.record()->dsUpdateTemplates.emplace_back(std::move(dsUpdateTemplate));

	auto dataSize = totalUpdateDataSize(*cmd.updateTemplate);
	cmd.data = copySpan(cb, reinterpret_cast<const std::byte*>(pData), dataSize);

	cb.dev->dispatch.CmdPushDescriptorSetWithTemplateKHR(commandBuffer,
		descriptorUpdateTemplate, layout, set, pData);
}

} // namespace vil
