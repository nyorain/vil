#include <queue.hpp>
#include <wrap.hpp>
#include <cb.hpp>
#include <ds.hpp>
#include <threadContext.hpp>
#include <command/commands.hpp>
#include <swapchain.hpp>
#include <sync.hpp>
#include <buffer.hpp>
#include <image.hpp>
#include <submit.hpp>
#include <deque>
#include <gui/gui.hpp>
#include <gui/commandHook.hpp>
#include <util/util.hpp>
#include <vk/enumString.hpp>
#include <util/profiling.hpp>

namespace vil {

std::optional<SubmIterator> checkLocked(SubmissionBatch& subm) {
	ZoneScoped;

	auto& dev = *subm.queue->dev;
	assertOwned(dev.mutex);

	if(subm.appFence) {
		if(dev.dispatch.GetFenceStatus(dev.handle, subm.appFence->handle) != VK_SUCCESS) {
			return std::nullopt;
		}
	} else {
		dlg_assert(subm.ourFence);
		if(dev.dispatch.GetFenceStatus(dev.handle, subm.ourFence) != VK_SUCCESS) {
			return std::nullopt;
		}
	}
	// apparently unique_ptr == ptr comparision not supported in stdlibc++ yet?
	auto it = std::find_if(dev.pending.begin(), dev.pending.end(), [&](auto& ptr){
			return ptr.get() == &subm;
	});
	dlg_assert(it != dev.pending.end());

	for(auto& sub : subm.submissions) {
		for(auto& [cb, hookData] : sub.cbs) {
			if(hookData) {
				hookData->finish(sub);
			}

			auto it2 = std::find(cb->pending.begin(), cb->pending.end(), &sub);
			dlg_assert(it2 != cb->pending.end());
			cb->pending.erase(it2);
		}

		// For a non-timeline semaphore (that was not waited upon), we
		// have to issue a vkQueueSubmit to reset them, we don't do that
		// immediately. We will do it with the next rendering or
		// when there are a lot of semaphores pending.
		if(sub.ourSemaphore && !dev.timelineSemaphores) {
			dev.resetSemaphores.push_back(sub.ourSemaphore);
		} else if(sub.ourSemaphore && dev.timelineSemaphores) {
			dev.semaphorePool.push_back(sub.ourSemaphore);
		}
	}

	if(subm.ourFence) {
		dev.dispatch.ResetFences(dev.handle, 1, &subm.ourFence);
		dev.fencePool.push_back(subm.ourFence);
	} else if(subm.appFence) {
		subm.appFence->submission = nullptr;
	}

	dev.semaphorePool.insert(dev.semaphorePool.end(),
		subm.poolSemaphores.begin(), subm.poolSemaphores.end());

	return dev.pending.erase(it);
}

SubmittedCommandBuffer::SubmittedCommandBuffer() = default;
SubmittedCommandBuffer::~SubmittedCommandBuffer() = default;

void checkPendingSubmissionsLocked(Device& dev) {
	assertOwned(dev.mutex);

	for(auto it = dev.pending.begin(); it != dev.pending.end();) {
		auto& subm = *it;
		auto nit = checkLocked(*subm);
		if(nit) {
			it = *nit;
			continue;
		}

		++it;
	}
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(
		VkQueue                                     queue,
		uint32_t                                    submitCount,
		const VkSubmitInfo*                         pSubmits,
		VkFence                                     fence) {
	ZoneScoped;
	auto& qd = getData<Queue>(queue);
	auto& dev = *qd.dev;

	QueueSubmitter submitter {};
	submitter.dev = &dev;
	submitter.queue = &qd;

	{
		// Get a new submission ID for this queue
		std::lock_guard lock(dev.mutex);
		submitter.globalSubmitID = ++dev.submissionCounter;

		// Check all pending submissions for completion, to possibly return
		// resources to fence/semaphore pools
		// check all submissions for completion
		checkPendingSubmissionsLocked(dev);
	}

	auto batchPtr = std::make_unique<SubmissionBatch>();
	auto& batch = *batchPtr;
	batch.submissions.reserve(submitCount); // make sure it's never re-allocated
	batch.queue = &qd;
	batch.globalSubmitID = submitter.globalSubmitID;

	submitter.dstBatch = &batch;

	process(submitter, {pSubmits, submitCount});

	// Make sure that every submission has a fence associated.
	// If the application already set a fence we can simply check that
	// to see if the submission completed (the vulkan spec gives us enough
	// guarantees to allow it). Otherwise we have to use a fence from the pool.
	VkFence submFence;
	if(fence) {
		batch.appFence = &dev.fences.get(fence);

		std::lock_guard lock(dev.mutex);

		// per vulkan spec, using a fence in QueueSubmit that is signaled
		// is not allowed. And if it was reset we also remove its associated
		// submission.
		dlg_assert(!batch.appFence->submission);
		submFence = fence;
		batch.appFence->submission = &batch;
	} else {
		// PERF: when we have timeline semaphores we can simply use our
		// added timeline semaphore to track this batch and don't need a fence at all.
		batch.ourFence = getFenceFromPool(dev);
		submFence = batch.ourFence;
	}

	VkResult res;

	// Lock order is important here, lock dev mutex before queue mutex.
	// We lock the dev mutex to sync with gui.
	{
		// TODO PERF: locking the dev mutex here is terrible for performance,
		// queueSubmit can take a long time and applications might parallelize
		// around it.
		// Maybe we can handle this with a separate gui/submission sync mutex?
		std::lock_guard devLock(dev.mutex);

		if(dev.doFullSync) {
			addFullSyncLocked(submitter);
		} else {
			addGuiSyncLocked(submitter);
		}

		{
			ZoneScopedN("dispatch.QueueSubmit");

			std::lock_guard queueLock(dev.queueMutex);
			res = dev.dispatch.QueueSubmit(qd.handle,
				u32(submitter.submitInfos.size()), submitter.submitInfos.data(),
				submFence);
		}

		if(res != VK_SUCCESS) {
			dlg_trace("vkQueueSubmit error: {}", vk::name(res));
			cleanupOnError(submitter);
			return res;
		}

		postProcessLocked(submitter);
		dev.pending.push_back(std::move(batchPtr));
	}

	// XXX: plain forward for debugging
	// auto res = dev.dispatch.QueueSubmit(qd.handle,
	// 	submitCount, pSubmits, fence);

	return res;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(VkQueue vkQueue) {
	auto& queue = getData<Queue>(vkQueue);
	VkResult res;

	{
		// waiting on a queue is considered a queue operation, needs
		// queue synchronization.
		std::lock_guard lock(queue.dev->queueMutex);
		res = queue.dev->dispatch.QueueWaitIdle(vkQueue);
		if(res != VK_SUCCESS) {
			return res;
		}
	}

	// check all submissions for completion
	std::lock_guard lock(queue.dev->mutex);
	for(auto it = queue.dev->pending.begin(); it != queue.dev->pending.end();) {
		auto& subm = *it;
		if(subm->queue != &queue) {
			++it;
			continue;
		}

		auto nit = checkLocked(*subm);
		if(!nit) {
			dlg_error("Expected submission to be completed after vkQueueWaitIdle");
			++it;
			continue;
		}

		it = *nit;
	}

	return res;
}

VkResult waitIdleImpl(Device& dev) {
	VkResult res;

	{
		// waiting on a device is considered a queue operation, needs
		// queue synchronization.
		std::lock_guard lock(dev.queueMutex);
		res = dev.dispatch.DeviceWaitIdle(dev.handle);
		if(res != VK_SUCCESS) {
			return res;
		}
	}

	// check all submissions for completion
	std::lock_guard lock(dev.mutex);
	for(auto it = dev.pending.begin(); it != dev.pending.end();) {
		auto& subm = *it;
		auto nit = checkLocked(*subm);
		if(nit) {
			it = *nit;
			continue;
		}

		dlg_error("Expected submission to be completed after vkDeviceWaitIdle");
		++it;
	}

	return res;
}

VKAPI_ATTR VkResult VKAPI_CALL DeviceWaitIdle(VkDevice device) {
	auto& dev = getDevice(device);
	return waitIdleImpl(dev);
}

// TODO: process the bindings. Wait until we know that the submission finished?
void process(Device& dev, ThreadMemScope& scope, VkSparseBufferMemoryBindInfo& bind) {
	auto& buf = get(dev, bind.buffer);
	bind.buffer = buf.handle;

	auto mems = scope.alloc<VkSparseMemoryBind>(bind.bindCount);
	for(auto i = 0u; i < bind.bindCount; ++i) {
		auto& b = mems[i];
		b = bind.pBinds[i];

		if(b.memory) {
			auto& mem = get(dev, b.memory);
			b.memory = mem.handle;
		}
	}

	bind.pBinds = mems.data();
}

void process(Device& dev, ThreadMemScope& scope, VkSparseImageOpaqueMemoryBindInfo& bind) {
	auto& img = get(dev, bind.image);
	bind.image = img.handle;

	auto mems = scope.alloc<VkSparseMemoryBind>(bind.bindCount);
	for(auto i = 0u; i < bind.bindCount; ++i) {
		auto& b = mems[i];
		b = bind.pBinds[i];

		if(b.memory) {
			auto& mem = get(dev, b.memory);
			b.memory = mem.handle;
		}
	}

	bind.pBinds = mems.data();
}

void process(Device& dev, ThreadMemScope& scope, VkSparseImageMemoryBindInfo& bind) {
	auto& img = get(dev, bind.image);
	bind.image = img.handle;

	auto mems = scope.alloc<VkSparseImageMemoryBind>(bind.bindCount);
	for(auto i = 0u; i < bind.bindCount; ++i) {
		auto& b = mems[i];
		b = bind.pBinds[i];

		if(b.memory) {
			auto& mem = get(dev, b.memory);
			b.memory = mem.handle;
		}
	}

	bind.pBinds = mems.data();
}

VKAPI_ATTR VkResult VKAPI_CALL QueueBindSparse(
		VkQueue                                     vkQueue,
		uint32_t                                    bindInfoCount,
		const VkBindSparseInfo*                     pBindInfo,
		VkFence                                     vkFence) {
	auto& queue = getData<Queue>(vkQueue);
	auto& dev = *queue.dev;

	Fence* fence {};
	if(vkFence) {
		fence = &get(dev, vkFence);
	}

	ThreadMemScope memScope;
	auto fwd = memScope.alloc<VkBindSparseInfo>(bindInfoCount);
	for(auto i = 0u; i < bindInfoCount; ++i) {
		auto& bindInfo = fwd[i];
		bindInfo = pBindInfo[i];

		// process semaphores
		auto waitSems = memScope.alloc<VkSemaphore>(bindInfo.waitSemaphoreCount);
		for(auto j = 0u; j < bindInfo.waitSemaphoreCount; ++j) {
			auto& sem = get(dev, bindInfo.pWaitSemaphores[j]);
			waitSems[j] = sem.handle;
		}

		auto signalSems = memScope.alloc<VkSemaphore>(bindInfo.signalSemaphoreCount);
		for(auto j = 0u; j < bindInfo.signalSemaphoreCount; ++j) {
			auto& sem = get(dev, bindInfo.pSignalSemaphores[j]);
			signalSems[j] = sem.handle;
		}

		// process bindings
		auto bufBinds = memScope.alloc<VkSparseBufferMemoryBindInfo>(bindInfo.bufferBindCount);
		for(auto j = 0u; j < bindInfo.bufferBindCount; ++j) {
			bufBinds[j] = bindInfo.pBufferBinds[j];
			process(dev, memScope, bufBinds[j]);
		}

		auto imgOpaqueBinds = memScope.alloc<VkSparseImageOpaqueMemoryBindInfo>(
			bindInfo.imageOpaqueBindCount);
		for(auto j = 0u; j < bindInfo.imageOpaqueBindCount; ++j) {
			imgOpaqueBinds[j] = bindInfo.pImageOpaqueBinds[j];
			process(dev, memScope, imgOpaqueBinds[j]);
		}

		auto imgBinds = memScope.alloc<VkSparseImageMemoryBindInfo>(bindInfo.imageBindCount);
		for(auto j = 0u; j < bindInfo.imageBindCount; ++j) {
			imgBinds[j] = bindInfo.pImageBinds[j];
			process(dev, memScope, imgBinds[j]);
		}

		bindInfo.pBufferBinds = bufBinds.data();
		bindInfo.pImageBinds = imgBinds.data();
		bindInfo.pImageOpaqueBinds = imgOpaqueBinds.data();
		bindInfo.pSignalSemaphores = signalSems.data();
		bindInfo.pWaitSemaphores = waitSems.data();
	}

	VkResult res;

	{
		// QueueBindSparse is a queue operation, have to lock our mutex
		std::lock_guard lock(queue.dev->queueMutex);
		res = queue.dev->dispatch.QueueBindSparse(queue.handle,
			u32(fwd.size()), fwd.data(), fence ? fence->handle : VK_NULL_HANDLE);
		if(res != VK_SUCCESS) {
			return res;
		}
	}

	// TODO: insert into pending submissions, track completion.
	// We might have to sync past and future hooks/gui submissions with this!

	return res;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(
		VkQueue                                     queue,
		uint32_t                                    submitCount,
		const VkSubmitInfo2*                        pSubmits,
		VkFence                                     fence) {
	(void) queue;
	(void) submitCount;
	(void) pSubmits;
	(void) fence;

	// Move the implentation of QueueSubmit here, adjust it for new infos.
	// And then make QueueSubmit just call this function.
	dlg_error("todo");
	return VK_ERROR_INCOMPATIBLE_DRIVER;
}

} // namespace vil
