#include <submit.hpp>
#include <layer.hpp>
#include <wrap.hpp>
#include <device.hpp>
#include <queue.hpp>
#include <image.hpp>
#include <buffer.hpp>
#include <ds.hpp>
#include <sync.hpp>
#include <cb.hpp>
#include <swapchain.hpp>
#include <util/ext.hpp>
#include <command/commands.hpp>
#include <util/util.hpp>
#include <util/intrusive.hpp>
#include <commandHook/hook.hpp>
#include <commandHook/record.hpp>
#include <commandHook/submission.hpp>
#include <gui/gui.hpp>
#include <vkutil/enumString.hpp>
#include <vk/format_utils.h>
#include <util/profiling.hpp>

namespace vil {

VkFence getFenceFromPool(Device& dev) {
	{
		std::lock_guard lock(dev.mutex);
		if(!dev.fencePool.empty()) {
			auto ret = dev.fencePool.back();
			dev.fencePool.pop_back();
			return ret;
		}
	}

	// create new fence
	VkFence fence;
	VkFenceCreateInfo fci {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VK_CHECK(dev.dispatch.CreateFence(dev.handle, &fci, nullptr, &fence));
	nameHandle(dev, fence, "Device:[pool fence]");
	return fence;
}

VkSemaphore createSemaphore(Device& dev) {
	// create new semaphore
	VkSemaphore semaphore;
	VkSemaphoreCreateInfo sci {};

	dlg_assert(!dev.timelineSemaphores);
	// VkSemaphoreTypeCreateInfo tsci {}; // keep-alive
	// if(dev.timelineSemaphores) {
	// 	tsci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
	// 	tsci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	// 	tsci.initialValue = 0u;
	// 	sci.pNext = &tsci;
	// }

	sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	VK_CHECK(dev.dispatch.CreateSemaphore(dev.handle, &sci, nullptr, &semaphore));
	nameHandle(dev, semaphore, "Device:[pool semaphore]");
	return semaphore;
}

VkSemaphore getSemaphoreFromPoolLocked(Device& dev) {
	if(!dev.semaphorePool.empty()) {
		auto ret = dev.semaphorePool.back();
		dev.semaphorePool.pop_back();
		return ret;
	}

	return createSemaphore(dev);
}


VkSemaphore getSemaphoreFromPool(Device& dev) {
	{
		std::unique_lock lock(dev.mutex);
		if(!dev.semaphorePool.empty()) {
			auto ret = dev.semaphorePool.back();
			dev.semaphorePool.pop_back();
			return ret;
		}
	}

	return createSemaphore(dev);
}

VkResult submitSemaphore(Queue& q, VkSemaphore sem, bool timeline) {
	VkSubmitInfo si {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.pSignalSemaphores = &sem;
	si.signalSemaphoreCount = 1u;

	VkTimelineSemaphoreSubmitInfo tsInfo {};
	if(timeline) {
		dlg_assert(q.dev->timelineSemaphores);

		u64 val;
		VK_CHECK(q.dev->dispatch.GetSemaphoreCounterValue(q.dev->handle, sem, &val));
		++val;

		tsInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		tsInfo.pSignalSemaphoreValues = &val;
		tsInfo.signalSemaphoreValueCount = 1u;
		si.pNext = &tsInfo;
	}

	std::lock_guard queueLock(q.dev->queueMutex);
	return q.dev->dispatch.QueueSubmit(q.handle, 1u, &si, VK_NULL_HANDLE);
}

void init(QueueSubmitter& subm, Queue& queue, SubmissionType type,
		VkFence fence) {
	auto& dev = *queue.dev;
	dlg_assert(!queue.createdByUs);

	subm.dev = &dev;
	subm.queue = &queue;

	{
		// Get a new submission ID for this queue
		std::lock_guard lock(dev.mutex);
		subm.globalSubmitID = ++dev.submissionCounter;

		// Check all pending submissions for completion, to possibly return
		// resources to fence/semaphore pools
		// check all submissions for completion
		checkPendingSubmissionsLocked(dev);
	}

	subm.dstBatch = std::make_unique<SubmissionBatch>();
	auto& batch = *subm.dstBatch;
	batch.type = type;
	batch.queue = &queue;
	batch.globalSubmitID = subm.globalSubmitID;

	// Make sure that every submission has a fence associated.
	// If the application already set a fence we can simply check that
	// to see if the submission completed (the vulkan spec gives us enough
	// guarantees to allow it). Otherwise we have to use a fence from the pool.
	if(fence) {
		batch.appFence = &dev.fences.get(fence);

		std::lock_guard lock(dev.mutex);

		// per vulkan spec, using a fence in QueueSubmit that is signaled
		// is not allowed. And if it was reset we also remove its associated
		// submission.
		dlg_assert(!batch.appFence->submission);
		subm.submFence = fence;
		batch.appFence->submission = &batch;
	} else {
		// PERF: when we have timeline semaphores we can simply use our
		// added timeline semaphore to track this batch and don't need a fence at all.
		batch.ourFence = getFenceFromPool(dev);
		subm.submFence = batch.ourFence;
	}
}

void process(QueueSubmitter& subm, VkSubmitInfo2& si) {
	ZoneScoped;

	auto& dev = *subm.dev;

	auto& dst = subm.dstBatch->submissions.emplace_back();
	dst.parent = subm.dstBatch.get();

	dlg_assert(!hasChain(si, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO));

	// = signal semaphores =
	auto fwdSignals = subm.memScope.alloc<VkSemaphoreSubmitInfo>(si.signalSemaphoreInfoCount);
	dst.signals.reserve(si.signalSemaphoreInfoCount);
	for(auto j = 0u; j < si.signalSemaphoreInfoCount; ++j) {
		auto& signal = si.pSignalSemaphoreInfos[j];

		auto& dstSync = *dst.signals.emplace_back(std::make_unique<SyncOp>());
		dstSync.semaphore = &get(dev, signal.semaphore);
		dstSync.stages = signal.stageMask;
		dstSync.submission = &dst;

		fwdSignals[j] = signal;
		fwdSignals[j].semaphore = dstSync.semaphore->handle;

		if(dstSync.semaphore->type == VK_SEMAPHORE_TYPE_TIMELINE) {
			dstSync.value = signal.value;
		}

		// we insert the submission into semaphore.signals in postProcess
	}

	si.signalSemaphoreInfoCount = fwdSignals.size();
	si.pSignalSemaphoreInfos = fwdSignals.data();

	// = wait semaphores =
	auto fwdWaits = subm.memScope.alloc<VkSemaphoreSubmitInfo>(si.waitSemaphoreInfoCount);
	dst.waits.reserve(si.waitSemaphoreInfoCount);
	for(auto j = 0u; j < si.waitSemaphoreInfoCount; ++j) {
		auto& wait = si.pWaitSemaphoreInfos[j];

		auto& dstSync = *dst.waits.emplace_back(std::make_unique<SyncOp>());
		dstSync.semaphore = &get(dev, wait.semaphore);
		dstSync.stages = wait.stageMask;
		dstSync.submission = &dst;

		fwdWaits[j] = wait;
		fwdWaits[j].semaphore = dstSync.semaphore->handle;

		if(dstSync.semaphore->type == VK_SEMAPHORE_TYPE_TIMELINE) {
			dstSync.value = wait.value;
		}

		// we insert the submission into semaphore.waits in postProcess
	}

	si.waitSemaphoreInfoCount = fwdWaits.size();
	si.pWaitSemaphoreInfos = fwdWaits.data();

	// = commandBuffers =
	auto fwdCbs = subm.memScope.alloc<VkCommandBufferSubmitInfo>(si.commandBufferInfoCount);
	auto& cmdSub = dst.data.emplace<CommandSubmission>();
	cmdSub.cbs.reserve(si.commandBufferInfoCount);
	for(auto j = 0u; j < si.commandBufferInfoCount; ++j) {
		auto& scb = cmdSub.cbs.emplace_back();
		scb.cb = &getCommandBuffer(si.pCommandBufferInfos[j].commandBuffer);

		fwdCbs[j] = si.pCommandBufferInfos[j];
		fwdCbs[j].commandBuffer = scb.cb->handle;
	}

	si.commandBufferInfoCount = fwdCbs.size();
	si.pCommandBufferInfos = fwdCbs.data();
}

void process(QueueSubmitter& subm, span<const VkSubmitInfo2> infos) {
	dlg_assert(subm.submitInfos.empty());
	dlg_assert(subm.dstBatch->submissions.empty());

	subm.submitInfos = subm.memScope.alloc<VkSubmitInfo2>(infos.size());
	subm.dstBatch->submissions.reserve(infos.size());

	for(auto i = 0u; i < infos.size(); ++i) {
		subm.submitInfos[i] = infos[i];
		process(subm, subm.submitInfos[i]);
	}

	if(subm.dev->commandHook) {
		// after this, the SubmitInfos might contain our hooked command
		// buffers, we must not use getCommandBuffer anymore!
		subm.dev->commandHook->hook(subm);
	}
}

// sparse
IntrusiveMemPtrBind<OpaqueSparseMemoryBind> process(MemoryResource& res,
		VkSparseMemoryBind& vkBind) {
	auto& dev = *res.dev;
	dlg_assert(res.memory.index() == 1u);

	OpaqueSparseMemoryBind bind {};
	bind.resource = &res;
	bind.flags = vkBind.flags;
	bind.memOffset = vkBind.memoryOffset;
	bind.memSize = vkBind.size;
	bind.resourceOffset = vkBind.resourceOffset;

	IntrusivePtr<DeviceMemory> memPtr;
	if(vkBind.memory) {
		memPtr = getPtr(dev, vkBind.memory);
		vkBind.memory = memPtr->handle;
		bind.memory = memPtr.get();
	}

	return {bind, memPtr};
}

VkDeviceSize memorySize(const Image& img, const VkSparseImageMemoryBind& bind) {
	auto& dev = *img.dev;

	// TODO: cache properties somewhere?
	u32 numFmtProps {};
	dev.ini->dispatch.GetPhysicalDeviceSparseImageFormatProperties(
		dev.phdev, img.ci.format, img.ci.imageType, img.ci.samples,
		img.ci.usage, img.ci.tiling, &numFmtProps, nullptr);

	ThreadMemScope tms;
	auto props = tms.alloc<VkSparseImageFormatProperties>(numFmtProps);
	dev.ini->dispatch.GetPhysicalDeviceSparseImageFormatProperties(
		dev.phdev, img.ci.format, img.ci.imageType, img.ci.samples,
		img.ci.usage, img.ci.tiling, &numFmtProps, props.data());

	VkExtent3D granularity {};
	for(auto& p : props) {
		if(p.aspectMask == bind.subresource.aspectMask) {
			granularity = p.imageGranularity;
			break;
		}
	}

	dlg_assert(granularity.width != 0);

	dlg_assert(bind.offset.x % granularity.width == 0);
	dlg_assert(bind.offset.y % granularity.height == 0);
	dlg_assert(bind.offset.z % granularity.depth == 0);

	auto blockSize = FormatTexelBlockExtent(img.ci.format);

	// compare vulkan pec: "Any bound partially-used-sparse-blocks must still
	// have their full sparse block size in bytes allocated in memory"
	// Granularity is the number of blocks for BC formats so this should be fine.
	auto numTexels =
		alignPOT(bind.extent.depth / blockSize.depth, granularity.depth) *
		alignPOT(bind.extent.height / blockSize.height, granularity.height) *
		alignPOT(bind.extent.width / blockSize.width, granularity.width);
	return numTexels * FormatElementSize(img.ci.format, bind.subresource.aspectMask);
}

IntrusiveMemPtrBind<ImageSparseMemoryBind> process(Image& img, VkSparseImageMemoryBind& vkBind) {
	auto& dev = *img.dev;

	dlg_assert(img.memory.index() == 1u);

	ImageSparseMemoryBind bind {};
	bind.resource = &img;
	bind.flags = vkBind.flags;
	bind.memOffset = vkBind.memoryOffset;
	bind.offset = vkBind.offset;
	bind.size = vkBind.extent;
	bind.subres = vkBind.subresource;
	bind.memSize = memorySize(img, vkBind);

	IntrusivePtr<DeviceMemory> memPtr;
	if(vkBind.memory) {
		memPtr = getPtr(dev, vkBind.memory);
		vkBind.memory = memPtr->handle;
		bind.memory = memPtr.get();
	}

	return {bind, memPtr};
}

void process(Device& dev, SparseBufferBind& dst, ThreadMemScope& scope,
		VkSparseBufferMemoryBindInfo& bind) {
	dst.dst.reset(&get(dev, bind.buffer));
	auto& buf = *dst.dst;
	bind.buffer = buf.handle;

	auto fwdBinds = scope.alloc<VkSparseMemoryBind>(bind.bindCount);
	dst.binds.reserve(bind.bindCount);
	for(auto i = 0u; i < bind.bindCount; ++i) {
		auto& b = fwdBinds[i];
		b = bind.pBinds[i];
		dst.binds.push_back(process(buf, b));
	}

	bind.pBinds = fwdBinds.data();
}

void process(Device& dev, SparseOpaqueImageBind& dst, ThreadMemScope& scope,
		VkSparseImageOpaqueMemoryBindInfo& bind) {
	dst.dst = getPtr(dev, bind.image);
	auto& img = *dst.dst;
	bind.image = img.handle;

	auto mems = scope.alloc<VkSparseMemoryBind>(bind.bindCount);
	dst.binds.reserve(bind.bindCount);
	for(auto i = 0u; i < bind.bindCount; ++i) {
		auto& b = mems[i];
		b = bind.pBinds[i];
		dst.binds.push_back(process(img, b));
	}

	bind.pBinds = mems.data();
}

void process(Device& dev, SparseImageBind& dst, ThreadMemScope& scope,
		VkSparseImageMemoryBindInfo& bind) {
	dst.dst = getPtr(dev, bind.image);
	auto& img = *dst.dst;
	bind.image = img.handle;

	auto mems = scope.alloc<VkSparseImageMemoryBind>(bind.bindCount);
	dst.binds.reserve(bind.bindCount);
	for(auto i = 0u; i < bind.bindCount; ++i) {
		auto& b = mems[i];
		b = bind.pBinds[i];
		dst.binds.push_back(process(img, b));
	}

	bind.pBinds = mems.data();
}


void process(QueueSubmitter& subm, VkBindSparseInfo& bi) {
	ZoneScoped;

	auto& memScope = subm.memScope;
	auto& dev = *subm.dev;

	auto& dst = subm.dstBatch->submissions.emplace_back();
	dst.parent = subm.dstBatch.get();

	auto tsInfo = findChainInfo<
		VkTimelineSemaphoreSubmitInfo,
		VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO>(bi);

	// = signal semaphores =
	auto fwdSignals = subm.memScope.alloc<VkSemaphore>(bi.signalSemaphoreCount);
	dst.signals.reserve(bi.signalSemaphoreCount);
	for(auto j = 0u; j < bi.signalSemaphoreCount; ++j) {
		auto& signal = bi.pSignalSemaphores[j];

		auto& dstSync = *dst.signals.emplace_back(std::make_unique<SyncOp>());
		dstSync.semaphore = &get(dev, signal);
		dstSync.stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		dstSync.submission = &dst;

		fwdSignals[j] = dstSync.semaphore->handle;

		if(dstSync.semaphore->type == VK_SEMAPHORE_TYPE_TIMELINE) {
			dlg_assert_or(tsInfo && j < tsInfo->signalSemaphoreValueCount, continue);
			dstSync.value = tsInfo->pSignalSemaphoreValues[j];
		}

		// we insert the submission into semaphore.signals in postProcess
	}

	bi.signalSemaphoreCount = fwdSignals.size();
	bi.pSignalSemaphores = fwdSignals.data();

	// = wait semaphores =
	auto fwdWaits = subm.memScope.alloc<VkSemaphore>(bi.waitSemaphoreCount);
	dst.waits.reserve(bi.waitSemaphoreCount);
	for(auto j = 0u; j < bi.waitSemaphoreCount; ++j) {
		auto& wait = bi.pWaitSemaphores[j];

		auto& dstSync = *dst.waits.emplace_back(std::make_unique<SyncOp>());
		dstSync.semaphore = &get(dev, wait);
		dstSync.stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		dstSync.submission = &dst;

		fwdWaits[j] = dstSync.semaphore->handle;

		if(dstSync.semaphore->type == VK_SEMAPHORE_TYPE_TIMELINE) {
			dlg_assert_or(tsInfo && j < tsInfo->waitSemaphoreValueCount, continue);
			dstSync.value = tsInfo->pWaitSemaphoreValues[j];
		}

		// we insert the submission into semaphore.waits in postProcess
	}

	bi.waitSemaphoreCount = fwdWaits.size();
	bi.pWaitSemaphores = fwdWaits.data();

	// process bindings
	auto& dstBinds = dst.data.emplace<BindSparseSubmission>();

	auto bufBinds = memScope.alloc<VkSparseBufferMemoryBindInfo>(bi.bufferBindCount);
	dstBinds.buffer.reserve(bi.bufferBindCount);
	for(auto j = 0u; j < bi.bufferBindCount; ++j) {
		bufBinds[j] = bi.pBufferBinds[j];
		process(dev, dstBinds.buffer.emplace_back(), memScope, bufBinds[j]);
	}

	auto imgOpaqueBinds = memScope.alloc<VkSparseImageOpaqueMemoryBindInfo>(
		bi.imageOpaqueBindCount);
	dstBinds.opaqueImage.reserve(bi.imageOpaqueBindCount);
	for(auto j = 0u; j < bi.imageOpaqueBindCount; ++j) {
		imgOpaqueBinds[j] = bi.pImageOpaqueBinds[j];
		process(dev, dstBinds.opaqueImage.emplace_back(), memScope, imgOpaqueBinds[j]);
	}

	auto imgBinds = memScope.alloc<VkSparseImageMemoryBindInfo>(bi.imageBindCount);
	dstBinds.image.reserve(bi.imageBindCount);
	for(auto j = 0u; j < bi.imageBindCount; ++j) {
		imgBinds[j] = bi.pImageBinds[j];
		process(dev, dstBinds.image.emplace_back(), memScope, imgBinds[j]);
	}

	bi.pBufferBinds = bufBinds.data();
	bi.pImageBinds = imgBinds.data();
	bi.pImageOpaqueBinds = imgOpaqueBinds.data();
}

void process(QueueSubmitter& subm, span<const VkBindSparseInfo> infos) {
	dlg_assert(subm.bindSparseInfos.empty());
	dlg_assert(subm.dstBatch->submissions.empty());

	subm.bindSparseInfos = subm.memScope.alloc<VkBindSparseInfo>(infos.size());
	subm.dstBatch->submissions.reserve(infos.size());

	for(auto i = 0u; i < infos.size(); ++i) {
		subm.bindSparseInfos[i] = infos[i];
		process(subm, subm.bindSparseInfos[i]);
	}
}

void addSubmissionSyncLocked(QueueSubmitter& subm) {
	assertOwned(subm.dev->mutex);

	for(auto [i, si] : enumerate(subm.submitInfos)) {
		auto& dst = subm.dstBatch->submissions[i];
		dst.queueSubmitID = ++subm.queue->submissionCounter;

		auto signalOps = subm.memScope.alloc<VkSemaphoreSubmitInfo>(si.signalSemaphoreInfoCount + 1);
		std::copy_n(si.pSignalSemaphoreInfos, si.signalSemaphoreInfoCount,
			signalOps.begin());

		auto& ourSignal = signalOps.back();
		ourSignal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
		ourSignal.stageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT; // all finished
		if(subm.dev->timelineSemaphores) {
			ourSignal.semaphore = subm.queue->submissionSemaphore;
			ourSignal.value = dst.queueSubmitID;
		} else {
			dst.ourSemaphore = getSemaphoreFromPoolLocked(*subm.dev);
			ourSignal.semaphore = dst.ourSemaphore;
		}

		si.signalSemaphoreInfoCount = u32(signalOps.size());
		si.pSignalSemaphoreInfos = signalOps.data();
	}
}

void cleanupOnErrorLocked(QueueSubmitter& subm) {
	auto& dev = *subm.dev;
	auto& batch = *subm.dstBatch;
	assertOwned(dev.mutex);

	if(batch.ourFence) {
		dev.fencePool.push_back(batch.ourFence);
	} else {
		dlg_assert(batch.appFence);
		batch.appFence->submission = nullptr;
	}

	// NOTE: this is potentially problematic in case we synced with the gfx
	// queue for gui as that semaphore is still pending
	// so waiting for it to reset it might actually have to wait
	dev.resetSemaphores.insert(dev.resetSemaphores.end(),
		batch.poolSemaphores.begin(), batch.poolSemaphores.end());

	if(subm.syncedGuiDraw) {
		// resetting this isn't only an optimization, we need to track
		// this to make sure to correctly reset it in future.
		subm.syncedGuiDraw->futureSemaphoreUsed = false;
	}

	for(auto& sub : batch.submissions) {
		if(sub.ourSemaphore) {
			dev.semaphorePool.push_back(sub.ourSemaphore);
		}
	}
}

void addFullSyncLocked(QueueSubmitter& subm) {
	ZoneScoped;

	auto& dev = *subm.dev;
	dlg_assert(dev.timelineSemaphores);

	const auto maxWaitSemCount = dev.queues.size();
	auto waitSems = subm.memScope.alloc<VkSemaphoreSubmitInfo>(maxWaitSemCount);
	auto waitSemCount = 0u;

	for(auto& pqueue : dev.queues) {
		auto& queue = *pqueue;
		if(&queue == subm.queue) {
			continue;
		}

		// no pending submissions on this queue
		u64 finishedID;
		dev.dispatch.GetSemaphoreCounterValue(dev.handle,
			queue.submissionSemaphore, &finishedID);
		if(finishedID == queue.submissionCounter) {
			continue;
		}

		dlg_assert(finishedID < queue.submissionCounter);
		if(subm.lastLayerSubmission) {
			// if subm.lastLayerSubmission was set, the submission we are currently
			// processing was hooked and we therefore need to sync with all
			// other queues.
			waitSems[waitSemCount].value = queue.submissionCounter;
		} else {
			// here we only need to sync with previous layer submissions
			// since we didn't insert any commands ourselves
			if(queue.lastLayerSubmission < finishedID) {
				continue;
			}

			waitSems[waitSemCount].value = queue.lastLayerSubmission;
		}

		waitSems[waitSemCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
		waitSems[waitSemCount].semaphore = queue.submissionSemaphore;
		waitSems[waitSemCount].stageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		++waitSemCount;
	}

	if(waitSemCount == 0u) {
		return;
	}

	// At this point we know that an additional submission is needed.
	// add it as *first* submission. Need to completely reallocate
	// span for that.
	auto newInfos = subm.memScope.alloc<VkSubmitInfo2>(subm.submitInfos.size() + 1);
	std::copy(subm.submitInfos.begin(), subm.submitInfos.end(), newInfos.begin() + 1);
	subm.submitInfos = newInfos;

	auto& si = subm.submitInfos[0];
	si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.pNext = nullptr;
	si.waitSemaphoreInfoCount = waitSemCount;
	si.pWaitSemaphoreInfos = waitSems.data();
}

void addGuiSyncLocked(QueueSubmitter& subm) {
	ZoneScoped;

	auto& dev = *subm.dev;
	auto& batch = *subm.dstBatch;

	Draw* insertGuiSync = nullptr;
	bool insertResetSemaphores = false;

	// first, check whether we need to insert an additional sync submission.

	// When this submission uses the gfxQueue (which we also use for
	// gui rendering) we don't have to synchronize with gui rendering
	// via semaphores, the pipeline barrier is enough.
	if(dev.guiLocked() && dev.gfxQueue != subm.queue) {
		// since all draws are submitted to the same queue
		// we only need to wait upon the last one pending.
		insertGuiSync = dev.guiLocked()->latestPendingDrawSyncLocked(batch);
	}

	// When we don't use timeline semaphores, we need to reset our
	// semaphore pool every now and then.
	dlg_assert(!dev.timelineSemaphores || dev.resetSemaphores.empty());
	if(dev.resetSemaphores.size() > 4) {
		insertResetSemaphores = true;
	}

	if(!insertResetSemaphores && !insertGuiSync) {
		return;
	}

	// At this point we know that an additional submission is needed.
	// add it as *first* submission. Need to completely reallocate
	// span for that.
	auto newInfos = subm.memScope.alloc<VkSubmitInfo2>(subm.submitInfos.size() + 1);
	std::copy(subm.submitInfos.begin(), subm.submitInfos.end(), newInfos.begin() + 1);
	subm.submitInfos = newInfos;

	auto& si = subm.submitInfos[0];
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.pNext = nullptr;

	auto maxWaitSemCount = dev.resetSemaphores.size() + 1;
	auto waitSems = subm.memScope.alloc<VkSemaphoreSubmitInfo>(maxWaitSemCount);

	for(auto i = 0u; i < dev.resetSemaphores.size(); ++i) {
		waitSems[i].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
		waitSems[i].semaphore = dev.resetSemaphores[i];
		// no actual waiting happening here, we just waot to reset them.
		waitSems[i].stageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
	}

	auto waitSemCount = dev.resetSemaphores.size();
	if(insertGuiSync) {
		auto& waitDraw = *insertGuiSync;
		dlg_assert(waitDraw.futureSemaphoreSignaled);
		waitSems[waitSemCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;

		// PERF(sync): when there are multiple submissions in the batch we could
		// sync on a per-submission basis (instead of per-batch). This is only
		// practical for timeline semaphores I guess since otherwise we can't
		// use the src semaphore multiple times.
		if(dev.timelineSemaphores) {
			auto& tsInfo = *subm.memScope.allocRaw<VkTimelineSemaphoreSubmitInfo>();
			tsInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
			tsInfo.pNext = NULL;

			auto waitVals = subm.memScope.alloc<u64>(1);
			waitSems[waitSemCount].semaphore = dev.guiLocked()->usedQueue().submissionSemaphore;
			waitSems[waitSemCount].value = waitDraw.lastSubmissionID;
			waitSems[waitSemCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			++waitSemCount;

			tsInfo.waitSemaphoreValueCount = u32(waitVals.size());
			tsInfo.pWaitSemaphoreValues = waitVals.data();

			si.pNext = &tsInfo;
		} else {
			if(waitDraw.futureSemaphoreUsed) {
				// we don't have timeline semaphores and have already
				// chained the future semaphore of that draw somewhere, ugh.
				// Add a new semaphore to the gui rendering queue.
				auto guiSyncSemaphore = getSemaphoreFromPoolLocked(dev);
				auto res = submitSemaphore(*subm.queue, guiSyncSemaphore);

				if(res != VK_SUCCESS) {
					dlg_error("internal vkQueueSubmit error: {} ({})",
						vk::name(res), res);

					// we continue as usual, the submission will just
					// not be chained correctly and might cause sync
					// issues.
				}

				waitSems[waitSemCount].semaphore = guiSyncSemaphore;
				waitSems[waitSemCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				++waitSemCount;

				batch.poolSemaphores.push_back(guiSyncSemaphore);
			} else {
				waitDraw.futureSemaphoreUsed = true;
				waitSems[waitSemCount].semaphore = waitDraw.futureSemaphore;
				waitSems[waitSemCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				// DON'T add waitDraw.futureSemaphore to batch.poolSemaphores
				// here since it's owned by the gui.

				subm.syncedGuiDraw = &waitDraw;
			}
		}
	}

	batch.poolSemaphores = std::move(dev.resetSemaphores);
	si.waitSemaphoreInfoCount = waitSemCount;
	si.pWaitSemaphoreInfos = waitSems.data();
}

void postProcessLocked(QueueSubmitter& subm) {
	ZoneScoped;

	auto& batch = *subm.dstBatch;

	// add to swapchain
	FrameSubmission* recordBatch = nullptr;
	auto swapchain = subm.dev->swapchainLocked();
	if(swapchain) {
		recordBatch = &swapchain->nextFrameSubmissions.batches.emplace_back();
		recordBatch->queue = subm.queue;
		recordBatch->submissionID = subm.globalSubmitID;
		recordBatch->type = batch.type;
	}

	for(auto& sub : batch.submissions) {
		// For command submissions, don't activate when there already is an
		// inactive submission on this queue since submission order blocks
		// further execution.
		// Sparse bindings don't depend on submission order.
		//
		// From the Vulkan spec 7.4.1:
		// Semaphore signal operations that are defined by vkQueueSubmit or
		// vkQueueSubmit2 additionally include all commands that occur earlier
		// in submission order. Semaphore signal operations that are defined by
		// vkQueueSubmit , vkQueueSubmit2 or vkQueueBindSparse additionally
		// include in the first synchronization scope any semaphore and fence
		// signal operations that occur earlier in signal operation order.
		bool doActivate = subm.dstBatch->type != SubmissionType::command ||
			!subm.queue->firstWaiting;

		// process cbs
		if(batch.type == SubmissionType::command) {
			auto& cmdSub = std::get<CommandSubmission>(sub.data);
			for(auto& scb : cmdSub.cbs) {
				auto* cb = scb.cb;
				// NOTE: could defer that to activate as well, not
				//   sure about it. Doing it here means we can never rely
				//   on cb->pending to be activated, i.e. must never wait
				//   on them in any way.
				cb->pending.push_back(&sub);
				auto recPtr = cb->lastRecordPtrLocked();

				if(recordBatch) {
					recordBatch->submissions.push_back(recPtr);
				}
			}
		} else if(batch.type == SubmissionType::bindSparse) {
			if(recordBatch) {
				recordBatch->sparseBinds.push_back(
					std::get<BindSparseSubmission>(sub.data));
			}
		} else {
			dlg_error("unreachable");
		}

		// insert into wait semaphores
		for(auto& dstSync : sub.waits) {
			dstSync->semaphore->waits.push_back(dstSync.get());

			if(dstSync->semaphore->type == VK_SEMAPHORE_TYPE_BINARY) {
				if(dstSync->semaphore->signals.empty()) {
					// TODO: validate that it's actually done
					// maybe insert doneDummy into signalOps and erase it
					// here?
					dstSync->counterpart = &SyncOp::doneDummy;
				} else {
					if(dstSync->semaphore->signals.back() != &SyncOp::swapchainAcquireDummy) {
						dstSync->semaphore->signals.back()->counterpart = dstSync.get();
					}
					dstSync->counterpart = dstSync->semaphore->signals.back();
				}

				// can't prevent activation via binary semaphores
			} else if(dstSync->value > dstSync->semaphore->upperBound) {
				doActivate = false;
			}
		}

		// insert into signal semaphores
		for(auto& dstSync : sub.signals) {
			dlg_checkt("validation",
				if(dstSync->semaphore->type == VK_SEMAPHORE_TYPE_BINARY) {
					auto& semaphore = *dstSync->semaphore;
					dlg_assert(semaphore.signals.empty() ||
						semaphore.signals.back()->counterpart ||
						(semaphore.signals.back() == &SyncOp::swapchainAcquireDummy &&
							!semaphore.waits.empty() &&
							semaphore.waits.back()->counterpart == &SyncOp::swapchainAcquireDummy));
				}
			);

			dstSync->semaphore->signals.push_back(dstSync.get());
		}

		if(doActivate) {
			activateLocked(sub);
		} else if(!subm.queue->firstWaiting) {
			subm.queue->firstWaiting = &sub;
		}
	}

	if(subm.lastLayerSubmission) {
		subm.queue->lastLayerSubmission = (*subm.lastLayerSubmission)->queueSubmitID;
	}
}

// Returns whether the given submission potentially writes the given
// DeviceHandle (only makes sense for Image and Buffer objects)
bool potentiallyWritesLocked(const Submission& subm, const Image* img, const Buffer* buf) {
	// TODO PERF: consider more information, not every use is potentially writing

	assertOwned(subm.parent->queue->dev->mutex);
	dlg_assert(img || buf);

	if(subm.parent->type == SubmissionType::command) {
		auto& cmdSub = std::get<CommandSubmission>(subm.data);
		for(auto& scb : cmdSub.cbs) {
			auto& cb = scb.cb;
			auto& rec = *cb->lastRecordLocked();

			if(buf) {
				auto it = find(rec.used.buffers, const_cast<Buffer&>(*buf));
				if(it != rec.used.buffers.end()) {
					return true;
				}
			} else if(img) {
				auto it = find(rec.used.images, const_cast<Image&>(*img));
				if(it != rec.used.images.end()) {
					return true;
				}
			} else {
				dlg_error("unreachable");
			}

			for(auto& uds : rec.used.descriptorSets) {
				// in this case we know that the bound descriptor set must
				// still be valid
				auto& state = *static_cast<DescriptorSet*>(uds.ds);
				// important that the ds mutex is locked mainly for
				// update_unused_while_pending.
				auto lock = state.lock();
				if((img && hasBound(state, *img)) || (buf && hasBound(state, *buf))) {
					return true;
				}
			}
		}
	} else {
		dlg_assert(subm.parent->type == SubmissionType::bindSparse);
		auto& bindSub = std::get<BindSparseSubmission>(subm.data);

		// TODO(correctness): also consider memory aliasing somehow?
		//   -> See aliasing in design.md

		for(auto& bufBind : bindSub.buffer) {
			if(buf && bufBind.dst == buf) {
				return true;
			}
		}

		for(auto& imgBind : bindSub.image) {
			if(img && imgBind.dst == img) {
				return true;
			}
		}

		for(auto& imgBind : bindSub.opaqueImage) {
			if(img && imgBind.dst == img) {
				return true;
			}
		}
	}

	return false;
}

std::vector<const Submission*> needsSyncLocked(const SubmissionBatch& pending, const Draw& draw) {
	ZoneScoped;

	auto& dev = *pending.queue->dev;
	if(pending.queue == dev.gfxQueue) {
		return {};
	}

	std::vector<const Submission*> subs;
	for(auto& subm : pending.submissions) {
		auto added = false;
		for(auto [handle, _layout] : draw.usedImages) {
			if(potentiallyWritesLocked(subm, handle, nullptr)) {
				subs.push_back(&subm);
				added = true;
				break;
			}
		}

		if(added) {
			continue;
		}

		for(auto* handle : draw.usedBuffers) {
			if(potentiallyWritesLocked(subm, nullptr, handle)) {
				subs.push_back(&subm);
				added = true;
				break;
			}
		}

		// TODO(correctness): also sync with memory objects.
		//   e.g. for aliasing. But we are only interested in specific ranges
		//   meh this will get complicated.
		//   -> See aliasing in design.md

		if(added) {
			continue;
		}

		// make sure that the draw has finished using the HookState if its
		// writen by the submission
		if(subm.parent->type == SubmissionType::command) {
			auto& cmdSub = std::get<CommandSubmission>(subm.data);
			for(auto& scb : cmdSub.cbs) {
				auto& hookPtr = scb.hook;
				// TODO(perf): we might not need sync in all cases. Pretty much only
				// for images and xfb buffers I guess.
				if(hookPtr && hookPtr->record->state.get() == draw.usedHookState.get()) {
					dlg_assert(hookPtr->record->writer == &subm);
					subs.push_back(&subm);
				}
			}
		}
	}

	return subs;
}

VkSubmitInfo2 upgrade(Device&, ThreadMemScope& tms, const VkSubmitInfo& si) {
	VkSubmitInfo2 ret {};
	ret.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;

	auto tsInfo = findChainInfo<
		VkTimelineSemaphoreSubmitInfo,
		VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO>(si);

	// cbs
	auto cbs = tms.alloc<VkCommandBufferSubmitInfo>(si.commandBufferCount);
	for(auto i = 0u; i < cbs.size(); ++i) {
		cbs[i].commandBuffer = si.pCommandBuffers[i];
	}

	ret.commandBufferInfoCount = u32(cbs.size());
	ret.pCommandBufferInfos = cbs.data();

	// waits
	auto waits = tms.alloc<VkSemaphoreSubmitInfo>(si.waitSemaphoreCount);
	for(auto i = 0u; i < waits.size(); ++i) {
		waits[i].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
		waits[i].semaphore = si.pWaitSemaphores[i];
		waits[i].stageMask = si.pWaitDstStageMask[i];

		if(tsInfo && i < tsInfo->waitSemaphoreValueCount) {
			waits[i].value = tsInfo->pWaitSemaphoreValues[i];
		}
	}

	ret.waitSemaphoreInfoCount = u32(waits.size());
	ret.pWaitSemaphoreInfos = waits.data();

	// signals
	auto signals = tms.alloc<VkSemaphoreSubmitInfo>(si.signalSemaphoreCount);
	for(auto i = 0u; i < signals.size(); ++i) {
		signals[i].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
		signals[i].semaphore = si.pSignalSemaphores[i];
		// default without sync2
		signals[i].stageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

		if(tsInfo && i < tsInfo->signalSemaphoreValueCount) {
			signals[i].value = tsInfo->pSignalSemaphoreValues[i];
		}
	}

	ret.signalSemaphoreInfoCount = u32(signals.size());
	ret.pSignalSemaphoreInfos = signals.data();

	return ret;
}

VkSubmitInfo downgrade(Device& dev, ThreadMemScope& tms, const VkSubmitInfo2& si) {
	VkSubmitInfo ret {};
	ret.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	VkTimelineSemaphoreSubmitInfo* tsInfo {};
	if(dev.timelineSemaphores) {
		dlg_assert(dev.timelineSemaphores);
		tsInfo = &tms.construct<VkTimelineSemaphoreSubmitInfo>();
		tsInfo->sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		tsInfo->pSignalSemaphoreValues = tms.allocRaw<u64>(si.signalSemaphoreInfoCount);
		tsInfo->pWaitSemaphoreValues = tms.allocRaw<u64>(si.waitSemaphoreInfoCount);
		tsInfo->signalSemaphoreValueCount = si.signalSemaphoreInfoCount;
		tsInfo->waitSemaphoreValueCount = si.waitSemaphoreInfoCount;
	}

	// cbs
	auto cbs = tms.alloc<VkCommandBuffer>(si.commandBufferInfoCount);
	for(auto i = 0u; i < cbs.size(); ++i) {
		cbs[i] = si.pCommandBufferInfos[i].commandBuffer;
	}

	ret.commandBufferCount = u32(cbs.size());
	ret.pCommandBuffers = cbs.data();

	// waits
	auto waits = tms.alloc<VkSemaphore>(si.waitSemaphoreInfoCount);
	auto waitStages = tms.alloc<VkPipelineStageFlags>(si.waitSemaphoreInfoCount);
	for(auto i = 0u; i < waits.size(); ++i) {
		waits[i] = si.pWaitSemaphoreInfos[i].semaphore;
		waitStages[i] = downgradePipelineStageFlags(si.pWaitSemaphoreInfos[i].stageMask);

		if(tsInfo) {
			const_cast<u64&>(tsInfo->pWaitSemaphoreValues[i]) =
				si.pWaitSemaphoreInfos[i].value;
		}
	}

	ret.waitSemaphoreCount = u32(waits.size());
	ret.pWaitSemaphores = waits.data();
	ret.pWaitDstStageMask = waitStages.data();

	// signals
	auto signals = tms.alloc<VkSemaphore>(si.signalSemaphoreInfoCount);
	for(auto i = 0u; i < signals.size(); ++i) {
		signals[i] = si.pSignalSemaphoreInfos[i].semaphore;
		// signal stage mask will implictly converted to bottom_of_pipe
		// nothing we can do
		dlg_assertl(dlg_level_warn,
			si.pSignalSemaphoreInfos[i].stageMask == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);

		if(tsInfo) {
			const_cast<u64&>(tsInfo->pSignalSemaphoreValues[i]) =
				si.pSignalSemaphoreInfos[i].value;
		}
	}

	ret.signalSemaphoreCount = u32(signals.size());
	ret.pSignalSemaphores = signals.data();

	if(tsInfo) {
		ret.pNext = tsInfo;
	}

	return ret;
}

} // vil
