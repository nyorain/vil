#include <cb.hpp>
#include <wrap.hpp>
#include <data.hpp>
#include <ds.hpp>
#include <queue.hpp>
#include <handles.hpp>
#include <accelStruct.hpp>
#include <threadContext.hpp>
#include <vk/typemap_helper.h>
#include <command/alloc.hpp>
#include <command/commands.hpp>
#include <gui/commandHook.hpp>
#include <util/util.hpp>
#include <util/ext.hpp>
#include <util/profiling.hpp>
#include <util/callstack.hpp>

namespace vil {

void copyChainInPlace(CommandBuffer& cb, const void*& pNext) {
	VkBaseInStructure* last = nullptr;
	auto it = static_cast<const VkBaseInStructure*>(pNext);
	pNext = nullptr;

	while(it) {
		auto size = structSize(it->sType);
		dlg_assertm_or(size > 0, it = it->pNext; continue,
			"Unknown structure type: {}", it->sType);

		auto& rec = nonNull(cb.builder().record_);
		auto buf = rec.alloc.allocate(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
		auto dst = reinterpret_cast<VkBaseInStructure*>(buf);
		// TODO: technicallly UB to not construct object via placement new.
		// In practice, this works everywhere since its only C PODs
		std::memcpy(dst, it, size);

		if(last) {
			last->pNext = dst;
		} else {
			pNext = dst;
		}

		last = dst;
		it = it->pNext;
	}
}

const void* copyChain(CommandBuffer& cb, const void* pNext) {
	auto ret = pNext;
	copyChainInPlace(cb, ret);
	return ret;
}

template<typename D, typename T>
void upgradeSpan(CommandBuffer& cb, span<D>& dst, T* data, size_t count) {
	dst = alloc<D>(cb, count);
	for(auto i = 0u; i < count; ++i) {
		dst[i] = upgrade(data[i]);
	}
}

void DescriptorState::bind(CommandBuffer& cb, PipelineLayout& layout, u32 firstSet,
		span<DescriptorSet* const> sets, span<const u32> dynOffsets) {
	auto allocSize = std::max<u32>(descriptorSets.size(), firstSet + sets.size());
	auto newSpan = allocUndef<BoundDescriptorSet>(cb, allocSize);
	std::copy(descriptorSets.begin(), descriptorSets.end(), newSpan.data());
	this->descriptorSets = newSpan;

	// NOTE: the "ds disturbing" part of vulkan is hard to grasp IMO.
	// There may be errors here.
	// TODO PERF: do we even need to track it like this? only useful if we
	// also show it in UI which sets were disturbed.
	// Disabled for now

// #define DS_DISTURB_CHECKS
#ifdef DS_DISTURB_CHECKS
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

	auto followingDisturbed = false;
#endif // DS_DISTURB_CHECKS

	for(auto i = 0u; i < sets.size(); ++i) {
		auto s = firstSet + i;
		auto& dsLayout = *layout.descriptors[s];

#ifdef DS_DISTURB_CHECKS
		if(!descriptorSets[s].layout || !compatibleForSetN(*descriptorSets[s].layout, layout, s)) {
			followingDisturbed = true;
		}
#endif // DS_DISTURB_CHECKS

		descriptorSets[s].layout = &layout;
		descriptorSets[s].dsEntry = sets[i]->setEntry;
		descriptorSets[s].dsID = sets[i]->id;
		descriptorSets[s].dsPool = sets[i]->pool;

		dlg_assert(dsLayout.numDynamicBuffers <= dynOffsets.size());
		descriptorSets[s].dynamicOffsets = copySpan(cb, dynOffsets.data(), dsLayout.numDynamicBuffers);
		dynOffsets.subspan(dsLayout.numDynamicBuffers);
	}

#ifdef DS_DISTURB_CHECKS
	if(followingDisturbed) {
		// dlg_debug("disturbed following descriptorSets, from {}", lastSet + 1);
		for(auto i = firstSet + sets.size(); i < descriptorSets.size(); ++i) {
			descriptorSets[i] = {};
		}
	}
#endif // DS_DISTURB_CHECKS
}

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

		if(builder_.record_) {
			dlg_assert(state_ == State::recording);
			dlg_assert(builder_.record_->cb == this);
			builder_.record_->cb = nullptr;
			builder_.record_->hookRecords.clear();
		}

		invalidateCbsLocked();
		notifyDestructionLocked(*dev, *this, VK_OBJECT_TYPE_COMMAND_BUFFER);
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

	dlg_assert(state_ == State::executable || pending.empty());

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

		// Make sure this command buffer isn't pending anymore
		clearPendingLocked();

		// if this was called while we were still recording, make
		// sure to properly terminate the pending record
		if(builder_.record_) {
			dlg_assert(state_ == State::recording);
			builder_.record_->cb = nullptr;
			builder_.record_->hookRecords.clear();
			keepAliveRecord = std::move(builder_.record_);
		}

		// if this command buffer holds an executable record, disconnect
		// it from this cb.
		if(lastRecord_ && lastRecord_->cb) {
			dlg_assert(lastRecord_->cb == this);
			lastRecord_->cb = nullptr;
			lastRecord_->hookRecords.clear();

			// we can't have an executable record *and* a recording one
			dlg_assert(!keepAliveRecord);
			keepAliveRecord = std::move(lastRecord_);
		}

		// We have to lock our own mutex since other threads might read
		// our data at the same time.
		if(startRecord) {
			++recordCount_;

			dlg_assert(!computeState_);
			dlg_assert(!graphicsState_);
			dlg_assert(!rayTracingState_);
			dlg_assert(!builder_.section_);

			builder_.reset(*this);
			state_ = CommandBuffer::State::recording;

			computeState_ = &construct<ComputeState>(*this);
			graphicsState_ = &construct<GraphicsState>(*this);
			rayTracingState_ = &construct<RayTracingState>(*this);
		} else {
			state_ = CommandBuffer::State::initial;
		}
	}
}

void CommandBuffer::doEnd() {
	ZoneScoped;
	dlg_assert(builder_.record_);
	dlg_assert(builder_.section_);

	// debug utils labels can be unterminated, see docs/debug-utils-label-nesting.md
	while(builder_.section_->parent) {
		auto lblCmd = dynamic_cast<const BeginDebugUtilsLabelCmd*>(builder_.section_->cmd);
		dlg_assert(lblCmd);
		dlg_assert(!builder_.section_->pop);
		builder_.record_->pushLables.push_back(lblCmd->name);
		builder_.section_ = builder_.section_->parent;
	}

	graphicsState_ = {};
	computeState_ = {};
	rayTracingState_ = {};
	pushConstants_ = {};

	builder_.section_ = nullptr;
	builder_.lastCommand_ = nullptr;
	ignoreEndDebugLabels_ = 0u;

	dlg_assert(subpass_ == u32(-1));
	dlg_assert(!rp_);
	dlg_assert(rpAttachments_.empty());
	subpass_ = u32(-1);
	rp_ = nullptr;
	rpAttachments_ = {};

	// parse commands into description
	// TODO: this should really be removed
	auto& rec = *builder_.record_;
	annotateRelIDLegacy(rec.commands);

	// Make sure to never call CommandBufferRecord destructor inside lock.
	// Don't just call reset() here or move lastRecord_ so that always have a valid
	// lastRecord_ as state (some other thread could query it before we lock)
	auto keepAliveRecord = lastRecord_;

	// Critical section
	{
		std::lock_guard lock(dev->mutex);
		ZoneScopedN("addToHandles");

		dlg_assert(state_ == State::recording);
		dlg_assert(!rec.finished);

		state_ = State::executable;

		for(auto& [handle, uh] : rec.handles) {
			if(handle->objectType == VK_OBJECT_TYPE_DESCRIPTOR_SET) {
				// special sentinel
				uh->next = uh;
				uh->prev = uh;
				continue;
			}

			if(handle->refRecords) {
				handle->refRecords->prev = uh;
			}
			uh->next = handle->refRecords;
			handle->refRecords = uh;
		}

		rec.finished = true;
		lastRecord_ = std::move(builder_.record_);
	}
}

void CommandBuffer::popLabelSections() {
	// See docs/debug-utils-label-nesting.md
	while(auto* next = dynamic_cast<BeginDebugUtilsLabelCmd*>(builder_.section_->cmd)) {
		dlg_trace("Problematic debug utils label nesting detected "
			"(Begin without end in scope): {}", next->name);
		builder_.record_->brokenHierarchyLabels = true;
		builder_.section_ = builder_.section_->parent;
		++ignoreEndDebugLabels_;
	}
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
	dlg_assert(lastRecord_.get());

	clearPendingLocked();

	// Free the hook data (as soon as possible), it's no longer
	// needed as this record will never be submitted again.
	lastRecord_->hookRecords.clear();
	lastRecord_->cb = nullptr;

	this->state_ = State::invalid;
}

ComputeState& CommandBuffer::newComputeState() {
	computeState_ = &construct<ComputeState>(*this, *computeState_);
	return *computeState_;
}

GraphicsState& CommandBuffer::newGraphicsState() {
	graphicsState_ = &construct<GraphicsState>(*this, *graphicsState_);
	return *graphicsState_;
}

RayTracingState& CommandBuffer::newRayTracingState() {
	rayTracingState_ = &construct<RayTracingState>(*this, *rayTracingState_);
	return *rayTracingState_;
}

CommandPool::~CommandPool() {
	if(!dev) {
		return;
	}

	invalidateCbs();
	notifyDestruction(*dev, *this, VK_OBJECT_TYPE_COMMAND_POOL);

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
template<typename T, SectionType ST = SectionType::none, typename... Args>
T& addCmd(CommandBuffer& cb, Args&&... args) {
	return cb.builder().add<T, ST>(std::forward<Args>(args)...);
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

void setup(CommandAlloc rec, RenderPassInstanceState& rpi, const RenderPassDesc& desc, unsigned subpass, span<ImageView*> attachments) {
	dlg_assert(subpass < desc.subpasses.size());
	auto& subpassDesc = desc.subpasses[subpass];

	// color
	rpi.colorAttachments = alloc<ImageView*>(rec, subpassDesc.colorAttachmentCount);
	for(auto i = 0u; i < subpassDesc.colorAttachmentCount; ++i) {
		auto& ref = subpassDesc.pColorAttachments[i];
		if(ref.attachment != VK_ATTACHMENT_UNUSED) {
			dlg_assert(ref.attachment < attachments.size());
			rpi.colorAttachments[i] = (attachments[ref.attachment]);
		}
	}

	// input
	rpi.inputAttachments = alloc<ImageView*>(rec, subpassDesc.inputAttachmentCount);
	for(auto i = 0u; i < subpassDesc.inputAttachmentCount; ++i) {
		auto& ref = subpassDesc.pInputAttachments[i];
		if(ref.attachment != VK_ATTACHMENT_UNUSED) {
			dlg_assert(ref.attachment < attachments.size());
			rpi.inputAttachments[i] = (attachments[ref.attachment]);
		}
	}

	// depthstencil
	if(subpassDesc.pDepthStencilAttachment) {
		auto& ref = *subpassDesc.pDepthStencilAttachment;
		if(ref.attachment != VK_ATTACHMENT_UNUSED) {
			dlg_assert(ref.attachment < attachments.size());
			rpi.depthStencilAttachment = attachments[ref.attachment];
		}
	}
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(
		VkCommandBuffer                             commandBuffer,
		const VkCommandBufferBeginInfo*             pBeginInfo) {
	auto& cb = getCommandBuffer(commandBuffer);
	cb.doReset(true);
	cb.builder().record_->usageFlags = pBeginInfo->flags;

	ThreadMemScope tms;
	VkCommandBufferInheritanceInfo inherit; // local copy, unwrapped
	auto beginInfo = *pBeginInfo;

	auto& gs = const_cast<GraphicsState&>(cb.graphicsState());

	if(pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) {
		dlg_assert(pBeginInfo->pInheritanceInfo);
		dlg_assert(pBeginInfo->pInheritanceInfo->renderPass);
		inherit = *pBeginInfo->pInheritanceInfo;

		auto* dynRender = LvlFindInChain<
			VkCommandBufferInheritanceRenderingInfo>(inherit.pNext);
		if(pBeginInfo->pInheritanceInfo->renderPass) {
			// get render pass
			auto& rp = get(*cb.dev, pBeginInfo->pInheritanceInfo->renderPass);
			inherit.renderPass = rp.handle;

			// get attachments
			span<ImageView*> attachments;
			if(pBeginInfo->pInheritanceInfo->framebuffer) {
				auto& fb = get(*cb.dev, pBeginInfo->pInheritanceInfo->framebuffer);

				// not sure if imageless framebuffers are allowed here.
				dlg_assert(!fb.imageless);
				attachments = fb.attachments;
				dlg_assert(rp.desc.attachments.size() == fb.attachments.size());

				inherit.framebuffer = fb.handle;
			}

			auto& rpi = construct<RenderPassInstanceState>(cb);
			// TODO: we don't call useHandle on the attachments, should do that
			setup(cb, rpi, rp.desc, inherit.subpass, attachments);
			gs.rpi = &rpi;

			// NOTE: no need to set cb.rp_, cb.subpass_, cb.rpAttachments_
			// here, they are only relevant for NextSubpass logic

			// TODO: use handles here? would have to allow using handles without command
			// Pretty sure we'd have to do it for correctness.
			// useHandle(cb, cmd, *cmd.fb);
			// useHandle(cb, cmd, *cmd.rp);
		} else if(dynRender) {
			// TODO: setup rpi. We can't do this since we don't have
			// view information here ugh. Should probably rework commands
			// to not reference rpi at all to support this. And instead
			// require knowing parent commands.
			// auto& dynRender = *const_cast<VkCommandBufferInheritanceRenderingInfo*>(
			// 	LvlFindInChain<VkCommandBufferInheritanceRenderingInfo>(inherit.pNext));
			// auto& rpi = construct<RenderPassInstanceState>(cb);
		} else {
			dlg_error("Unsupported VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT usage");
		}

		beginInfo.pInheritanceInfo = &inherit;
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
auto& useHandleImpl(CommandRecord& rec, Command& cmd, DeviceHandle& handle) {
	auto it = rec.handles.find(&handle);
	if(it == rec.handles.end()) {
		auto& uh = construct<UsedHandle>(rec, rec);
		it = rec.handles.emplace(&handle, &uh).first;
	}

	it->second->commands.push_back(&cmd);
	return *it->second;
}

UsedImage& useHandleImpl(CommandRecord& rec, Command& cmd, Image& img) {
	auto it = rec.handles.find(&img);
	if(it == rec.handles.end()) {
		auto& uh = construct<UsedImage>(rec, rec);
		it = rec.handles.emplace(&img, &uh).first;
	}

	it->second->commands.push_back(&cmd);

	return static_cast<UsedImage&>(*it->second);
}

auto& useHandle(CommandRecord& rec, Command& cmd, DeviceHandle& handle) {
	return useHandleImpl(rec, cmd, handle);
}

UsedImage& useHandle(CommandRecord& rec, Command& cmd, Image& img) {
	auto& ui = useHandleImpl(rec, cmd, img);

	// NOTE: add swapchain in case it's a swapchain image?
	// shouldn't be needed I guess.
	// NOTE: can currently fail for sparse bindings i guess
	dlg_assert(img.memory || img.swapchain);
	if(img.memory) {
		useHandle(rec, cmd, *img.memory);
	}

	return ui;
}

auto& useHandle(CommandRecord& rec, Command& cmd, ImageView& view, bool useImg = true) {
	auto& ret = useHandle(rec, cmd, static_cast<DeviceHandle&>(view));
	dlg_assert(view.img);
	if(useImg && view.img) {
		useHandle(rec, cmd, *view.img);
	}
	return ret;
}

auto& useHandle(CommandRecord& rec, Command& cmd, Buffer& buf) {
	auto& ret = useHandle(rec, cmd, static_cast<DeviceHandle&>(buf));

	// NOTE: can currently fail for sparse bindings i guess
	dlg_assert(buf.memory);
	if(buf.memory) {
		useHandle(rec, cmd, *buf.memory);
	}

	return ret;
}

auto& useHandle(CommandRecord& rec, Command& cmd, BufferView& view) {
	auto& ret = useHandle(rec, cmd, static_cast<DeviceHandle&>(view));

	dlg_assert(view.buffer);
	if(view.buffer) {
		useHandle(rec, cmd, *view.buffer);
	}

	return ret;
}

auto& useHandle(CommandRecord& rec, Command& cmd, Image& image, VkImageLayout newLayout) {
	auto& img = useHandle(rec, cmd, image);
	img.layoutChanged = true;
	img.finalLayout = newLayout;
	return img;
}

template<typename... Args>
auto& useHandle(CommandBuffer& cb, Args&&... args) {
	dlg_assert(cb.state() == CommandBuffer::State::recording);
	dlg_assert(cb.builder().record_);
	return useHandle(*cb.builder().record_, std::forward<Args>(args)...);
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
	cmd.recordQueueFamilyIndex = cb.pool().queueFamily;

	cmd.images = alloc<Image*>(cb, cmd.imgBarriers.size());
	for(auto i = 0u; i < cmd.imgBarriers.size(); ++i) {
		auto& imgb = cmd.imgBarriers[i];
		copyChainInPlace(cb, imgb.pNext);

		auto& img = get(*cb.dev, imgb.image);
		cmd.images[i] = &img;

		auto& usedImage = useHandle(cb, cmd, img, imgb.newLayout);
		if(imgb.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
			// this is the only layout transition case that can irreversibly
			// alter the images contents
			usedImage.potentiallyWrites = true;
		}

		imgb.image = img.handle;
	}

	cmd.buffers = alloc<Buffer*>(cb, cmd.bufBarriers.size());
	for(auto i = 0u; i < cmd.bufBarriers.size(); ++i) {
		auto& bufb = cmd.bufBarriers[i];
		copyChainInPlace(cb, bufb.pNext);

		auto& buf = get(*cb.dev, bufb.buffer);
		cmd.buffers[i] = &buf;
		useHandle(cb, cmd, buf);

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

	cmd.events = alloc<Event*>(cb, eventCount);
	for(auto i = 0u; i < eventCount; ++i) {
		auto& event = get(*cb.dev, pEvents[i]);
		cmd.events[i] = &event;
		useHandle(cb, cmd, event);
	}

	cmd.record(*cb.dev, cb.handle());
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

	cmd.record(*cb.dev, cb.handle());
}

void cmdBeginRenderPass(CommandBuffer& cb,
		VkRenderPassBeginInfo& rpBeginInfo,
		const VkSubpassBeginInfo& subpassBeginInfo, ThreadMemScope& memScope) {
	auto& cmd = addCmd<BeginRenderPassCmd, SectionType::begin>(cb);

	// copy the chain, we might have to modify it below
	rpBeginInfo.pNext = copyChainLocal(memScope, rpBeginInfo.pNext);

	cmd.clearValues = copySpan(cb, rpBeginInfo.pClearValues, rpBeginInfo.clearValueCount);
	cmd.info = rpBeginInfo;
	cmd.info.pClearValues = cmd.clearValues.data();
	copyChainInPlace(cb, cmd.info.pNext);

	cmd.fb = &get(*cb.dev, rpBeginInfo.framebuffer);
	cmd.rp = &get(*cb.dev, rpBeginInfo.renderPass);

	cmd.subpassBeginInfo = subpassBeginInfo;
	copyChainInPlace(cb, cmd.subpassBeginInfo.pNext);

	useHandle(cb, cmd, *cmd.fb);
	useHandle(cb, cmd, *cmd.rp);

	if(cmd.fb->imageless) {
		constexpr auto sType = VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO;
		auto* cAttInfo = findChainInfo<VkRenderPassAttachmentBeginInfo, sType>(rpBeginInfo);
		// we can const_cast here since we copied the chain above
		auto* attInfo = const_cast<VkRenderPassAttachmentBeginInfo*>(cAttInfo);
		dlg_assert(attInfo);

		dlg_assert(cmd.rp->desc.attachments.size() == attInfo->attachmentCount);

		// NOTE: we allocate from cb here because of dstAttInfo below.
		auto fwdAttachments = alloc<VkImageView>(cb, attInfo->attachmentCount);
		cmd.attachments = alloc<ImageView*>(cb, attInfo->attachmentCount);

		for(auto i = 0u; i < attInfo->attachmentCount; ++i) {
			auto& attachment = get(*cb.dev, attInfo->pAttachments[i]);
			dlg_assert(attachment.img);

			useHandle(cb, cmd, attachment, false);
			useHandle(cb, cmd, nonNull(attachment.img),
				cmd.rp->desc.attachments[i].finalLayout);

			cmd.attachments[i] = &attachment;
			fwdAttachments[i] = attachment.handle;
		}

		attInfo->pAttachments = fwdAttachments.data();

		// TODO: kinda hacky, we need a better general solution to deep-copy
		// and unwrap handles in extension chain structs
		auto* cDstAttInfo = findChainInfo<VkRenderPassAttachmentBeginInfo, sType>(cmd.info);
		// we can const_cast here since we copied the chain above
		auto* dstAttInfo = const_cast<VkRenderPassAttachmentBeginInfo*>(cDstAttInfo);
		dlg_assert(dstAttInfo);

		dstAttInfo->pAttachments = fwdAttachments.data();
		dlg_assert(dstAttInfo->attachmentCount == fwdAttachments.size());
	} else {
		dlg_assert(cmd.rp->desc.attachments.size() == cmd.fb->attachments.size());
		cmd.attachments = alloc<ImageView*>(cb, cmd.fb->attachments.size());

		for(auto i = 0u; i < cmd.fb->attachments.size(); ++i) {
			auto& attachment = cmd.fb->attachments[i];
			dlg_assert(attachment && attachment->img);

			cmd.attachments[i] = attachment;
			useHandle(cb, cmd, *attachment, false);
			useHandle(cb, cmd, *attachment->img,
				cmd.rp->desc.attachments[i].finalLayout);
		}
	}

	dlg_assert(!cb.rp_);
	cb.subpass_ = 0u;
	cb.rp_ = cmd.rp;
	cb.rpAttachments_ = cmd.attachments;

	setup(cb, cmd.rpi, cmd.rp->desc, 0u, cmd.attachments);

	auto& gs = cb.newGraphicsState();
	dlg_assert(!gs.rpi);
	gs.rpi = &cmd.rpi;

	auto& subpassCmd = addCmd<FirstSubpassCmd, SectionType::begin>(cb);
	(void) subpassCmd;

	rpBeginInfo.framebuffer = cmd.fb->handle;
	rpBeginInfo.renderPass = cmd.rp->handle;
}

void cmdEndRenderPass(CommandBuffer& cb, const VkSubpassEndInfo& endInfo) {
	cb.popLabelSections();
	dlg_assert(cb.builder().section_ &&
		dynamic_cast<SubpassCmd*>(cb.builder().section_->cmd));

	cb.builder().endSection(nullptr); // pop subpass section
	dlg_assert(cb.builder().section_ &&
		dynamic_cast<BeginRenderPassCmd*>(cb.builder().section_->cmd));

	auto& cmd = addCmd<EndRenderPassCmd, SectionType::end>(cb);
	cmd.endInfo = endInfo;
	copyChainInPlace(cb, cmd.endInfo.pNext);

	auto& gs = cb.newGraphicsState();
	dlg_assert(gs.rpi);
	gs.rpi = {};

	cb.subpass_ = u32(-1);
	cb.rp_ = nullptr;
	cb.rpAttachments_ = {};
}

void cmdNextSubpass(CommandBuffer& cb, const VkSubpassBeginInfo& beginInfo, const VkSubpassEndInfo& endInfo) {
	cb.popLabelSections();
	dlg_assert(cb.builder().section_ &&
		dynamic_cast<SubpassCmd*>(cb.builder().section_->cmd));

	auto& cmd = addCmd<NextSubpassCmd, SectionType::next>(cb);
	cmd.beginInfo = beginInfo;
	copyChainInPlace(cb, cmd.beginInfo.pNext);

	cmd.endInfo = endInfo;
	copyChainInPlace(cb, cmd.endInfo.pNext);

	++cb.subpass_;
	cmd.subpassID = cb.subpass_;
	setup(cb, cmd.rpi, cb.rp_->desc, cb.subpass_, cb.rpAttachments_);

	auto& gs = cb.newGraphicsState();
	dlg_assert(gs.rpi);
	gs.rpi = &cmd.rpi;
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(
		VkCommandBuffer                             commandBuffer,
		const VkRenderPassBeginInfo*                pRenderPassBegin,
		VkSubpassContents                           contents) {
	auto& cb = getCommandBuffer(commandBuffer);

	VkSubpassBeginInfo subpassBeginInfo = {};
	subpassBeginInfo.contents = contents;
	subpassBeginInfo.sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO;

	ThreadMemScope memScope;
	auto beginInfo = *pRenderPassBegin;
	cmdBeginRenderPass(cb, beginInfo, subpassBeginInfo, memScope);

	cb.dev->dispatch.CmdBeginRenderPass(cb.handle(), &beginInfo, contents);
}

VKAPI_ATTR void VKAPI_CALL CmdNextSubpass(
		VkCommandBuffer                             commandBuffer,
		VkSubpassContents                           contents) {
	auto& cb = getCommandBuffer(commandBuffer);
	cmdNextSubpass(cb,
		{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO, nullptr, contents},
		{VK_STRUCTURE_TYPE_SUBPASS_END_INFO, nullptr});
	cb.dev->dispatch.CmdNextSubpass(cb.handle(), contents);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(
		VkCommandBuffer                             commandBuffer) {
	auto& cb = getCommandBuffer(commandBuffer);
	cmdEndRenderPass(cb, {VK_STRUCTURE_TYPE_SUBPASS_END_INFO, nullptr});
	cb.dev->dispatch.CmdEndRenderPass(cb.handle());
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass2(
		VkCommandBuffer                             commandBuffer,
		const VkRenderPassBeginInfo*                pRenderPassBegin,
		const VkSubpassBeginInfo*                   pSubpassBeginInfo) {
	auto& cb = getCommandBuffer(commandBuffer);

	ThreadMemScope memScope;
	auto beginInfo = *pRenderPassBegin;
	cmdBeginRenderPass(cb, beginInfo, *pSubpassBeginInfo, memScope);

	cb.dev->dispatch.CmdBeginRenderPass2(cb.handle(), &beginInfo, pSubpassBeginInfo);
}

VKAPI_ATTR void VKAPI_CALL CmdNextSubpass2(
		VkCommandBuffer                             commandBuffer,
		const VkSubpassBeginInfo*                   pSubpassBeginInfo,
		const VkSubpassEndInfo*                     pSubpassEndInfo) {
	auto& cb = getCommandBuffer(commandBuffer);
	cmdNextSubpass(cb, *pSubpassBeginInfo, *pSubpassEndInfo);
	cb.dev->dispatch.CmdNextSubpass2(cb.handle(), pSubpassBeginInfo, pSubpassEndInfo);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass2(
		VkCommandBuffer                             commandBuffer,
		const VkSubpassEndInfo*                     pSubpassEndInfo) {
	auto& cb = getCommandBuffer(commandBuffer);
	cmdEndRenderPass(cb, *pSubpassEndInfo);
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
	cb.builder().record_->pipeLayouts.emplace_back(std::move(pipeLayoutPtr));

	cmd.sets = alloc<DescriptorSet*>(cb, descriptorSetCount);

	ThreadMemScope memScope;
	auto setHandles = memScope.allocUndef<VkDescriptorSet>(descriptorSetCount);
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
		cb.newComputeState().bind(cb, *cmd.pipeLayout, firstSet, cmd.sets,
			{pDynamicOffsets, pDynamicOffsets + dynamicOffsetCount});
	} else if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
		cb.newGraphicsState().bind(cb, *cmd.pipeLayout, firstSet, cmd.sets,
			{pDynamicOffsets, pDynamicOffsets + dynamicOffsetCount});
	} else if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) {
		cb.newRayTracingState().bind(cb, *cmd.pipeLayout, firstSet, cmd.sets,
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

	auto& gs = cb.newGraphicsState();
	gs.indices.buffer = &buf;
	gs.indices.offset = offset;
	gs.indices.type = indexType;

	cb.dev->dispatch.CmdBindIndexBuffer(cb.handle(), buf.handle, offset, indexType);
}

span<VkBuffer> cmdBindVertexBuffers(CommandBuffer& cb, ThreadMemScope& tms,
		uint32_t                                    firstBinding,
		uint32_t                                    bindingCount,
		const VkBuffer*                             pBuffers,
		const VkDeviceSize*                         pOffsets,
		const VkDeviceSize*                         pSizes,
		const VkDeviceSize*                         pStrides) {

	auto& cmd = addCmd<BindVertexBuffersCmd>(cb);
	cmd.firstBinding = firstBinding;

	auto& gs = cb.newGraphicsState();
	ensureSize(cb, gs.vertices, firstBinding + bindingCount);
	cmd.buffers = alloc<BoundVertexBuffer>(cb, bindingCount);

	auto bufHandles = tms.alloc<VkBuffer>(bindingCount);
	for(auto i = 0u; i < bindingCount; ++i) {
		auto& buf = get(*cb.dev, pBuffers[i]);
		cmd.buffers[i].buffer = &buf;
		cmd.buffers[i].offset = pOffsets[i];
		cmd.buffers[i].size = pSizes ? pSizes[i] : 0u;
		cmd.buffers[i].stride = pStrides ? pStrides[i] : 0u;
		useHandle(cb, cmd, buf);

		gs.vertices[firstBinding + i] = cmd.buffers[i];
		bufHandles[i] = buf.handle;
	}

	return bufHandles;
}

VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    firstBinding,
		uint32_t                                    bindingCount,
		const VkBuffer*                             pBuffers,
		const VkDeviceSize*                         pOffsets) {
	auto& cb = getCommandBuffer(commandBuffer);
	ThreadMemScope tms;
	auto bufHandles = cmdBindVertexBuffers(cb, tms, firstBinding, bindingCount,
		pBuffers, pOffsets, nullptr, nullptr);
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
	auto& cmd = addCmd<DrawCmd>(cb, cb);

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
	auto& cmd = addCmd<DrawIndexedCmd>(cb, cb);

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
	auto& cmd = addCmd<DrawIndirectCmd>(cb, cb);

	auto& buf = get(*cb.dev, buffer);
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
	auto& cmd = addCmd<DrawIndirectCmd>(cb, cb);

	auto& buf = get(*cb.dev, buffer);
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
	auto& cmd = addCmd<DrawIndirectCountCmd>(cb, cb);

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
	auto& cmd = addCmd<DrawIndirectCountCmd>(cb, cb);

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
	auto& cmd = addCmd<DispatchCmd>(cb, cb);

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
	auto& cmd = addCmd<DispatchIndirectCmd>(cb, cb);
	cmd.offset = offset;

	auto& buf = get(*cb.dev, buffer);
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
	auto& cmd = addCmd<DispatchBaseCmd>(cb, cb);

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

	auto& src = get(*cb.dev, srcImage);
	auto& dst = get(*cb.dev, dstImage);

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

VKAPI_ATTR void VKAPI_CALL CmdCopyImage2(
		VkCommandBuffer                             commandBuffer,
		const VkCopyImageInfo2KHR*                  info) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<CopyImageCmd>(cb);

	auto& src = get(*cb.dev, info->srcImage);
	auto& dst = get(*cb.dev, info->dstImage);

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

	auto& src = get(*cb.dev, srcImage);
	auto& dst = get(*cb.dev, dstImage);

	cmd.src = &src;
	cmd.srcLayout = srcImageLayout;
	cmd.dst = &dst;
	cmd.dstLayout = dstImageLayout;
	upgradeSpan(cb, cmd.blits, pRegions, regionCount);
	cmd.filter = filter;

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst).potentiallyWrites = true;

	cb.dev->dispatch.CmdBlitImage(cb.handle(),
		src.handle, srcImageLayout,
		dst.handle, dstImageLayout,
		regionCount, pRegions, filter);
}

VKAPI_ATTR void VKAPI_CALL CmdBlitImage2(
		VkCommandBuffer                             commandBuffer,
		const VkBlitImageInfo2KHR*                  info) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<BlitImageCmd>(cb);

	auto& src = get(*cb.dev, info->srcImage);
	auto& dst = get(*cb.dev, info->dstImage);

	cmd.pNext = copyChain(cb, info->pNext);
	cmd.src = &src;
	cmd.srcLayout = info->srcImageLayout;
	cmd.dst = &dst;
	cmd.dstLayout = info->dstImageLayout;
	cmd.blits = copySpan(cb, info->pRegions, info->regionCount);
	cmd.filter = info->filter;

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst).potentiallyWrites = true;

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

	auto& src = get(*cb.dev, srcBuffer);
	auto& dst = get(*cb.dev, dstImage);

	cmd.src = &src;
	cmd.dst = &dst;
	cmd.dstLayout = dstImageLayout;
	upgradeSpan(cb, cmd.copies, pRegions, regionCount);

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst).potentiallyWrites = true;

	cb.dev->dispatch.CmdCopyBufferToImage(cb.handle(),
		src.handle, dst.handle, dstImageLayout, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyBufferToImage2(
		VkCommandBuffer                             commandBuffer,
		const VkCopyBufferToImageInfo2KHR*          info) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<CopyBufferToImageCmd>(cb);

	auto& src = get(*cb.dev, info->srcBuffer);
	auto& dst = get(*cb.dev, info->dstImage);

	cmd.pNext = copyChain(cb, info->pNext);
	cmd.src = &src;
	cmd.dst = &dst;
	cmd.dstLayout = info->dstImageLayout;
	cmd.copies = copySpan(cb, info->pRegions, info->regionCount);

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst).potentiallyWrites = true;

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

	auto& src = get(*cb.dev, srcImage);
	auto& dst = get(*cb.dev, dstBuffer);

	cmd.src = &src;
	cmd.dst = &dst;
	cmd.srcLayout = srcImageLayout;
	upgradeSpan(cb, cmd.copies, pRegions, regionCount);

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst);

	cb.dev->dispatch.CmdCopyImageToBuffer(cb.handle(),
		src.handle, srcImageLayout, dst.handle, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyImageToBuffer2(
		VkCommandBuffer                             commandBuffer,
		const VkCopyImageToBufferInfo2KHR*          info) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<CopyImageToBufferCmd>(cb);

	auto& src = get(*cb.dev, info->srcImage);
	auto& dst = get(*cb.dev, info->dstBuffer);

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

	auto& dst = get(*cb.dev, image);
	cmd.dst = &dst;
	cmd.color = *pColor;
	cmd.dstLayout = imageLayout;
	cmd.ranges = copySpan(cb, pRanges, rangeCount);

	useHandle(cb, cmd, dst).potentiallyWrites = true;

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

	auto& dst = get(*cb.dev, image);
	cmd.dst = &dst;
	cmd.dstLayout = imageLayout;
	cmd.value = *pDepthStencil;
	cmd.ranges = copySpan(cb, pRanges, rangeCount);

	useHandle(cb, cmd, dst).potentiallyWrites = true;

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

	dlg_assert(cb.graphicsState().rpi);
	cmd.rpi = cb.graphicsState().rpi;

	// NOTE: We explicitly don't add the cleared attachments to used handles here.
	// In case of a secondary command buffer with dynamic rendering, we might not even know them.

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
	auto& src = get(*cb.dev, srcImage);
	auto& dst = get(*cb.dev, dstImage);

	auto& cmd = addCmd<ResolveImageCmd>(cb);
	cmd.src = &src;
	cmd.srcLayout = srcImageLayout;
	cmd.dst = &dst;
	cmd.dstLayout = dstImageLayout;
	upgradeSpan(cb, cmd.regions, pRegions, regionCount);

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst).potentiallyWrites = true;

	cb.dev->dispatch.CmdResolveImage(cb.handle(), src.handle, srcImageLayout,
		dst.handle, dstImageLayout, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL CmdResolveImage2(
		VkCommandBuffer                             commandBuffer,
		const VkResolveImageInfo2KHR*               info) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& src = get(*cb.dev, info->srcImage);
	auto& dst = get(*cb.dev, info->dstImage);

	auto& cmd = addCmd<ResolveImageCmd>(cb);

	cmd.pNext = copyChain(cb, info->pNext);
	cmd.src = &src;
	cmd.srcLayout = info->srcImageLayout;
	cmd.dst = &dst;
	cmd.dstLayout = info->dstImageLayout;
	cmd.regions = copySpan(cb, info->pRegions, info->regionCount);

	useHandle(cb, cmd, src);
	useHandle(cb, cmd, dst).potentiallyWrites = true;

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
	auto& parentRec = *cb.builder().record_;
	cb.builder().appendParent(cmd);

	cmd.stats_.numChildSections = commandBufferCount;
	auto* last = static_cast<ExecuteCommandsChildCmd*>(nullptr);

	ThreadMemScope memScope;
	auto cbHandles = memScope.alloc<VkCommandBuffer>(commandBufferCount);
	for(auto i = 0u; i < commandBufferCount; ++i) {
		auto& secondary = getCommandBuffer(pCommandBuffers[i]);
		dlg_assert(secondary.state() == CommandBuffer::State::executable);

		// We don't have to lock the mutex here because the command buffer
		// state is not allowed to change while it is used here.
		auto recordPtr = secondary.lastRecordPtrLocked();

		auto& childCmd = construct<ExecuteCommandsChildCmd>(cb);
		childCmd.id_ = i;
		childCmd.record_ = recordPtr.get();

		if(!last) {
			dlg_assert(!cmd.children_);
			cmd.children_ = &childCmd;
		} else {
			dlg_assert(cmd.children_);
			last->next = &childCmd;
			last->nextParent_ = &childCmd;
		}

		// Needed to correctly invalidate cb when a secondary buffer is
		// reset/destroyed.
		useHandle(cb, cmd, secondary);

		auto& rec = *recordPtr;
		for(auto& [handle, uh] : rec.handles) {
			// Make sure we carry along layout changes done in secondary
			// command buffers.
			if(handle->objectType == VK_OBJECT_TYPE_IMAGE) {
				auto& ui = static_cast<UsedImage&>(*uh);
				auto& dst = useHandleImpl(parentRec, cmd, static_cast<Image&>(*handle));
				dst.potentiallyWrites |= ui.potentiallyWrites;

				if(ui.layoutChanged) {
					dst.layoutChanged = true;
					dst.finalLayout = ui.finalLayout;
				}

				continue;
			}

			useHandleImpl(parentRec, cmd, *handle);
		}

		cb.builder().record_->secondaries.push_back(std::move(recordPtr));
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

VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer2(
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

	// Create without SectionType::end, we end the section below
	// manually if possible.
	addCmd<EndDebugUtilsLabelCmd>(cb);

	// See docs/debug-utils-label-nesting.md
	// When the last section isn't a BeginDebugUtilsLabelCmd, we have to
	// find the last one in the cb.section_ stack and mark it for pop.
	auto* section = cb.builder().section_;
	if(cb.ignoreEndDebugLabels() > 0) {
		--cb.ignoreEndDebugLabels();
	} else if(section && dynamic_cast<const BeginDebugUtilsLabelCmd*>(section->cmd)) {
		cb.builder().endSection(nullptr);
	} else {
		auto* it = section;
		while(it) {
			auto lcmd = dynamic_cast<BeginDebugUtilsLabelCmd*>(it->cmd);
			if(lcmd && !it->pop) {
				dlg_trace("Problematic debug utils label nesting detected (End)");
				it->pop = true;
				cb.builder().record_->brokenHierarchyLabels = true;
				break;
			}

			it = it->parent;
		}

		// If there is no active label section at all, the command buffer
		// effectively pops it from the queue.
		if(!it) {
			++cb.builder().record_->numPopLabels;
		}
	}

	if(cb.dev->dispatch.CmdEndDebugUtilsLabelEXT) {
		cb.dev->dispatch.CmdEndDebugUtilsLabelEXT(cb.handle());
	}
}

VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(
		VkCommandBuffer                             commandBuffer,
		VkPipelineBindPoint                         pipelineBindPoint,
		VkPipeline                                  pipeline) {
	ExtZoneScoped;

	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<BindPipelineCmd>(cb);
	cmd.bindPoint = pipelineBindPoint;

	auto& pipe = get(*cb.dev, pipeline);
	dlg_assert(pipe.type == pipelineBindPoint);

	if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
		cb.newComputeState().pipe = static_cast<ComputePipeline*>(&pipe);
		cmd.pipe = cb.computeState().pipe;
		useHandle(cb, cmd, *cmd.pipe);
	} else if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
		cb.newGraphicsState().pipe = static_cast<GraphicsPipeline*>(&pipe);
		cmd.pipe = cb.graphicsState().pipe;
		useHandle(cb, cmd, *cmd.pipe);
	} else if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) {
		cb.newRayTracingState().pipe = static_cast<RayTracingPipeline*>(&pipe);
		cmd.pipe = cb.rayTracingState().pipe;
		useHandle(cb, cmd, *cmd.pipe);
	} else {
		dlg_error("unknown pipeline bind point");
	}

	// add pipeline to stats
	auto& stats = cb.builder().section_->cmd->stats_;
	auto& node = construct<ParentCommand::SectionStats::BoundPipeNode>(cb);
	node.next = stats.boundPipelines;
	node.pipe = &pipe;
	stats.boundPipelines = &node;
	++stats.numPipeBinds;

	{
		ExtZoneScopedN("dispatch");
		cb.dev->dispatch.CmdBindPipeline(cb.handle(), pipelineBindPoint, pipe.handle);
	}
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
	cb.builder().record_->pipeLayouts.emplace_back(std::move(pipeLayoutPtr));

	cmd.stages = stageFlags;
	cmd.offset = offset;
	auto ptr = static_cast<const std::byte*>(pValues);
	cmd.values = copySpan(cb, static_cast<const std::byte*>(ptr), size);

	// reallocate push constants
	auto& pc = cb.pushConstants().data;
	auto allocSize = std::max<u32>(offset + size, pc.size());
	pc = alloc<std::byte>(cb, allocSize);
	std::memcpy(pc.data() + offset, pValues, size);

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

	auto& gs = cb.newGraphicsState();
	ensureSize(cb, gs.dynamic.viewports, firstViewport + viewportCount);
	std::copy(pViewports, pViewports + viewportCount,
		gs.dynamic.viewports.begin() + firstViewport);

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

	auto& gs = cb.newGraphicsState();
	ensureSize(cb, gs.dynamic.scissors, firstScissor + scissorCount);
	std::copy(pScissors, pScissors + scissorCount,
		gs.dynamic.scissors.begin() + firstScissor);

	cb.dev->dispatch.CmdSetScissor(cb.handle(), firstScissor, scissorCount, pScissors);
}

VKAPI_ATTR void VKAPI_CALL CmdSetLineWidth(
		VkCommandBuffer                             commandBuffer,
		float                                       lineWidth) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetLineWidthCmd>(cb);
	cmd.width = lineWidth;

	auto& gs = cb.newGraphicsState();
	gs.dynamic.lineWidth = lineWidth;
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

	auto& gs = cb.newGraphicsState();
	gs.dynamic.depthBias = cmd.state;

	cb.dev->dispatch.CmdSetDepthBias(cb.handle(),
		depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
}

VKAPI_ATTR void VKAPI_CALL CmdSetBlendConstants(
		VkCommandBuffer                             commandBuffer,
		const float                                 blendConstants[4]) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetBlendConstantsCmd>(cb);
	std::memcpy(cmd.values.data(), blendConstants, sizeof(cmd.values));

	auto& gs = cb.newGraphicsState();
	std::memcpy(gs.dynamic.blendConstants.data(), blendConstants,
		sizeof(gs.dynamic.blendConstants));

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

	auto& gs = cb.newGraphicsState();
	gs.dynamic.depthBoundsMin = minDepthBounds;
	gs.dynamic.depthBoundsMax = maxDepthBounds;

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

	auto& gs = cb.newGraphicsState();
	if(faceMask & VK_STENCIL_FACE_FRONT_BIT) {
		gs.dynamic.stencilFront.compareMask = compareMask;
	}
	if(faceMask & VK_STENCIL_FACE_BACK_BIT) {
		gs.dynamic.stencilBack.compareMask = compareMask;
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

	auto& gs = cb.newGraphicsState();
	if(faceMask & VK_STENCIL_FACE_FRONT_BIT) {
		gs.dynamic.stencilFront.compareMask = writeMask;
	}
	if(faceMask & VK_STENCIL_FACE_BACK_BIT) {
		gs.dynamic.stencilBack.compareMask = writeMask;
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

	auto& gs = cb.newGraphicsState();
	if(faceMask & VK_STENCIL_FACE_FRONT_BIT) {
		gs.dynamic.stencilFront.reference = reference;
	}
	if(faceMask & VK_STENCIL_FACE_BACK_BIT) {
		gs.dynamic.stencilBack.reference = reference;
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
				auto copies = alloc<VkDescriptorBufferInfo>(cb, write.descriptorCount);
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
				auto copies = alloc<VkDescriptorImageInfo>(cb, write.descriptorCount);
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
				auto copies = alloc<VkBufferView>(cb, write.descriptorCount);
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
	cb.builder().record_->pipeLayouts.emplace_back(std::move(pipeLayoutPtr));

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
	cb.builder().record_->pipeLayouts.emplace_back(std::move(pipeLayoutPtr));

	dlg_assert(set < cmd.pipeLayout->descriptors.size());
	auto& dsLayout = *cmd.pipeLayout->descriptors[set];

	auto dsUpdateTemplate = getPtr(*cb.dev, descriptorUpdateTemplate);
	cmd.updateTemplate = dsUpdateTemplate.get();
	cb.builder().record_->dsUpdateTemplates.emplace_back(std::move(dsUpdateTemplate));

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
	ThreadMemScope tms;
	auto bufHandles = cmdBindVertexBuffers(cb, tms, firstBinding, bindingCount,
		pBuffers, pOffsets, pSizes, pStrides);
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

VKAPI_ATTR void VKAPI_CALL CmdSetPatchControlPointsEXT(
		VkCommandBuffer                             commandBuffer,
		uint32_t                                    patchControlPoints) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetPatchControlPointsCmd>(cb);
	cmd.patchControlPoints = patchControlPoints;

	cb.dev->dispatch.CmdSetPatchControlPointsEXT(cb.handle(), patchControlPoints);
}

VKAPI_ATTR void VKAPI_CALL CmdSetRasterizerDiscardEnableEXT(
		VkCommandBuffer                             commandBuffer,
		VkBool32                                    rasterizerDiscardEnable) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetRasterizerDiscardEnableCmd>(cb);
	cmd.enable = rasterizerDiscardEnable;

	cb.dev->dispatch.CmdSetRasterizerDiscardEnableEXT(cb.handle(), rasterizerDiscardEnable);
}

VKAPI_ATTR void VKAPI_CALL CmdSetDepthBiasEnableEXT(
		VkCommandBuffer                             commandBuffer,
		VkBool32                                    depthBiasEnable) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetDepthBiasEnableCmd>(cb);
	cmd.enable = depthBiasEnable;

	cb.dev->dispatch.CmdSetDepthBiasEnableEXT(cb.handle(), depthBiasEnable);
}

VKAPI_ATTR void VKAPI_CALL CmdSetLogicOpEXT(
		VkCommandBuffer                             commandBuffer,
		VkLogicOp                                   logicOp) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetLogicOpCmd>(cb);
	cmd.logicOp = logicOp;

	cb.dev->dispatch.CmdSetDepthBiasEnableEXT(cb.handle(), logicOp);
}

VKAPI_ATTR void VKAPI_CALL CmdSetPrimitiveRestartEnableEXT(
		VkCommandBuffer                             commandBuffer,
		VkBool32                                    primitiveRestartEnable) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<SetPrimitiveRestartEnableCmd>(cb);
	cmd.enable = primitiveRestartEnable;

	cb.dev->dispatch.CmdSetDepthBiasEnableEXT(cb.handle(), primitiveRestartEnable);
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

	cmd.srcs = alloc<AccelStruct*>(cb, infoCount);
	cmd.dsts = alloc<AccelStruct*>(cb, infoCount);
	cmd.buildRangeInfos = alloc<span<VkAccelerationStructureBuildRangeInfoKHR>>(cb, infoCount);

	cmd.buildInfos = copySpan(cb, pInfos, infoCount);
	for(auto i = 0u; i < infoCount; ++i) {
		auto& buildInfo = cmd.buildInfos[i];
		copyChainInPlace(cb, buildInfo.pNext);

		// We have to perform a deep-copy of the geometry descriptions.
		if(buildInfo.pGeometries) {
			buildInfo.pGeometries = copySpan(cb, buildInfo.pGeometries, buildInfo.geometryCount).data();
		} else if(buildInfo.geometryCount > 0) {
			dlg_assert(buildInfo.ppGeometries);
			auto dst = alloc<VkAccelerationStructureGeometryKHR>(cb, buildInfo.geometryCount);
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

	// we need to set this flag here so we can hook that record when it
	// is submitted to the gpu to retrieve the actual data used for building.
	cb.builder().record_->buildsAccelStructs = true;

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

	cmd.srcs = alloc<AccelStruct*>(cb, infoCount);
	cmd.dsts = alloc<AccelStruct*>(cb, infoCount);
	cmd.indirectAddresses = alloc<VkDeviceAddress>(cb, infoCount);
	cmd.indirectStrides = alloc<u32>(cb, infoCount);
	cmd.maxPrimitiveCounts = alloc<u32*>(cb, infoCount);

	cmd.buildInfos = copySpan(cb, pInfos, infoCount);
	for(auto i = 0u; i < infoCount; ++i) {
		auto& buildInfo = cmd.buildInfos[i];
		copyChainInPlace(cb, buildInfo.pNext);

		// We have to perform a deep-copy of the geometry descriptions.
		if(buildInfo.pGeometries) {
			buildInfo.pGeometries = copySpan(cb, buildInfo.pGeometries, buildInfo.geometryCount).data();
		} else if(buildInfo.geometryCount > 0) {
			dlg_assert(buildInfo.ppGeometries);
			auto dst = alloc<VkAccelerationStructureGeometryKHR>(cb, buildInfo.geometryCount);
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

		cmd.indirectAddresses[i] = pIndirectDeviceAddresses[i];
		cmd.indirectStrides[i] = pIndirectStrides[i];
		cmd.maxPrimitiveCounts[i] = copySpan(cb,
			ppMaxPrimitiveCounts[i], buildInfo.geometryCount).data();

		// TODO: useHandle for buffers of associated device addresses?
	}

	// we need to set this flag here so we can hook that record when it
	// is submitted to the gpu to retrieve the actual data used for building.
	cb.builder().record_->buildsAccelStructs = true;

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
	fwd.dst = cmd.dst->handle;

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

	cmd.accelStructs = alloc<AccelStruct*>(cb, accelerationStructureCount);

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
	auto& cmd = addCmd<TraceRaysCmd>(cb, cb);
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
	auto& cmd = addCmd<TraceRaysIndirectCmd>(cb, cb);
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

VKAPI_ATTR void VKAPI_CALL CmdBeginRendering(
		VkCommandBuffer                             commandBuffer,
		const VkRenderingInfo*                      pRenderingInfo) {
	auto& cb = getCommandBuffer(commandBuffer);

	auto& info = *pRenderingInfo;
	auto& cmd = addCmd<BeginRenderingCmd, SectionType::begin>(cb);
	cmd.layerCount = info.layerCount;
	cmd.viewMask = info.viewMask;
	cmd.flags = info.flags;
	cmd.renderArea = info.renderArea;

	auto set = [&](const VkRenderingAttachmentInfo& src,
			BeginRenderingCmd::Attachment& dst) {
		if(!src.imageView) {
			return static_cast<ImageView*>(nullptr);
		}

		dst.view = &unwrap(src.imageView);
		useHandle(cb, cmd, *dst.view);

		dst.imageLayout = src.imageLayout;
		dst.resolveImageLayout = src.resolveImageLayout;
		dst.resolveMode = src.resolveMode;
		dst.clearValue = src.clearValue;
		dst.loadOp = src.loadOp;
		dst.storeOp = src.storeOp;

		if(src.resolveImageView) {
			dst.resolveView = &unwrap(src.resolveImageView);
			useHandle(cb, cmd, *dst.resolveView);
		}

		return dst.view;
	};

	cmd.colorAttachments = alloc<BeginRenderingCmd::Attachment>(cb,
		info.colorAttachmentCount);
	cmd.rpi.colorAttachments = alloc<ImageView*>(cb, info.colorAttachmentCount);
	for(auto i = 0u; i < cmd.colorAttachments.size(); ++i) {
		auto* view = set(info.pColorAttachments[i], cmd.colorAttachments[i]);
		cmd.rpi.colorAttachments[i] = view;
	}

	if(info.pDepthAttachment) {
		auto* view = set(*info.pDepthAttachment, cmd.depthAttachment);
		cmd.rpi.depthStencilAttachment = view;
	}

	if(info.pStencilAttachment) {
		auto* view = set(*info.pStencilAttachment, cmd.stencilAttachment);
		dlg_assert(!cmd.rpi.depthStencilAttachment ||
			cmd.rpi.depthStencilAttachment == view);
		cmd.rpi.depthStencilAttachment = view;
	}

	auto& gs = cb.newGraphicsState();
	dlg_assert(!gs.rpi);
	gs.rpi = &cmd.rpi;

	cmd.record(*cb.dev, cb.handle());
}

VKAPI_ATTR void VKAPI_CALL CmdEndRendering(
		VkCommandBuffer                             commandBuffer) {
	auto& cb = getCommandBuffer(commandBuffer);
	auto& cmd = addCmd<EndRenderingCmd, SectionType::end>(cb);
	(void) cmd;

	cb.dev->dispatch.CmdEndRenderingKHR(cb.handle());
}

} // namespace vil
