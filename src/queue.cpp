#include <queue.hpp>
#include <wrap.hpp>
#include <cb.hpp>
#include <ds.hpp>
#include <layer.hpp>
#include <memory.hpp>
#include <threadContext.hpp>
#include <command/commands.hpp>
#include <swapchain.hpp>
#include <sync.hpp>
#include <buffer.hpp>
#include <image.hpp>
#include <submit.hpp>
#include <gui/gui.hpp>
#include <commandHook/submission.hpp>
#include <util/util.hpp>
#include <vkutil/enumString.hpp>
#include <util/profiling.hpp>

namespace vil {

void finish(SubmissionBatch& batch) {
	auto& dev = *batch.queue->dev;

	for(auto& sub : batch.submissions) {
		dlg_assert(sub.active);

		// process finished records
		if(batch.type == SubmissionType::command) {
			auto& cmdSub = std::get<CommandSubmission>(sub.data);
			for(auto& scb : cmdSub.cbs) {
				if(scb.hook) {
					scb.hook->finish(sub);
				} else {
					auto& accelStructCopies = scb.cb->lastRecordLocked()->accelStructCopies;
					dlg_assert(accelStructCopies.size() == scb.accelStructCopies.size());
					for(auto [i, copy] : enumerate(accelStructCopies)) {
						copy.dst->lastValid = scb.accelStructCopies[i];
					}
				}

				auto it2 = std::find(scb.cb->pending.begin(), scb.cb->pending.end(), &sub);
				dlg_assert(it2 != scb.cb->pending.end());
				scb.cb->pending.erase(it2);
			}
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

		// process waits
		for(auto& wait : sub.waits) {
			auto& sem = *wait->semaphore;

			if(sem.type == VK_SEMAPHORE_TYPE_BINARY) {
				dlg_assert(wait->counterpart);
				if(wait->counterpart &&
						wait->counterpart != &SyncOp::doneDummy &&
						wait->counterpart != &SyncOp::swapchainAcquireDummy) {
					dlg_assert(wait->counterpart->submission);
					wait->counterpart->counterpart = &SyncOp::doneDummy;
				}
				if(wait->counterpart == &SyncOp::swapchainAcquireDummy) {
					// in case there are multiple swapchain acquire signals, just
					// erase the first one, does not matter
					auto it = std::find(sem.signals.begin(), sem.signals.end(),
						&SyncOp::swapchainAcquireDummy);
					dlg_assert(it != sem.signals.end());
					sem.signals.erase(it);
				}
			}

			auto it = std::find(sem.waits.begin(), sem.waits.end(), wait.get());
			dlg_assert(it != sem.waits.end());
			sem.waits.erase(it);
		}

		// process signals
		for(auto& signal : sub.signals) {
			auto& sem = *signal->semaphore;

			if(sem.type == VK_SEMAPHORE_TYPE_BINARY) {
				if(signal->counterpart &&
						signal->counterpart != &SyncOp::doneDummy &&
						signal->counterpart != &SyncOp::queuePresentDummy) {
					dlg_assert(signal->counterpart->submission);
					signal->counterpart->counterpart = &SyncOp::doneDummy;
				}
			} else {
				sem.lowerBound = std::max(sem.lowerBound, signal->value);
				dlg_assert(sem.lowerBound <= sem.upperBound);
			}

			auto it = std::find(sem.signals.begin(), sem.signals.end(), signal.get());
			dlg_assert(it != sem.signals.end());
			sem.signals.erase(it);
		}
	}
}

std::optional<SubmIterator> checkLocked(SubmissionBatch& batch) {
	ZoneScoped;

	auto& dev = *batch.queue->dev;
	assertOwned(dev.mutex);

	// If a submission in the batch isn't even active yet, it can't
	// be finished. We don't have to check it, but it's an optimization.
	// NOTE: this is important for integration testing though. The
	//   mock icd always returns VK_SUCCESS for fences.
	for(auto& sub : batch.submissions) {
		if(!sub.active) {
			return std::nullopt;
		}
	}

	if(batch.appFence) {
		auto res = dev.dispatch.GetFenceStatus(dev.handle, batch.appFence->handle);
		if(res != VK_SUCCESS) {
			if(res == VK_ERROR_DEVICE_LOST) {
				onDeviceLost(dev);
			}

			return std::nullopt;
		}
	} else {
		dlg_assert(batch.ourFence);
		auto res = dev.dispatch.GetFenceStatus(dev.handle, batch.ourFence);
		if(res != VK_SUCCESS) {
			if(res == VK_ERROR_DEVICE_LOST) {
				onDeviceLost(dev);
			}

			return std::nullopt;
		}
	}
	// apparently unique_ptr == ptr comparision not supported in stdlibc++ yet?
	auto finder = [&](auto& ptr){ return ptr.get() == &batch; };
	auto it = std::find_if(dev.pending.begin(), dev.pending.end(), finder);
	dlg_assert(it != dev.pending.end());

	finish(batch);

	if(batch.ourFence) {
		dev.dispatch.ResetFences(dev.handle, 1, &batch.ourFence);
		dev.fencePool.push_back(batch.ourFence);
	} else if(batch.appFence) {
		batch.appFence->submission = nullptr;
	}

	dev.semaphorePool.insert(dev.semaphorePool.end(),
		batch.poolSemaphores.begin(), batch.poolSemaphores.end());

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

VkResult doSubmit(Queue& queue, span<const VkSubmitInfo2> submits,
		VkFence fence, bool legacy) {
	ZoneScoped;
	auto& dev = *queue.dev;

	checkInitWindow(dev);

	QueueSubmitter submitter {};
	init(submitter, queue, SubmissionType::command, fence);

	process(submitter, submits);

	VkResult res;

	// Lock order is important here, lock dev mutex before queue mutex.
	// We lock the dev mutex to sync with gui.
	{
		// TODO PERF: locking the dev mutex here is terrible for performance,
		// queueSubmit can take a long time and applications might parallelize
		// around it.
		// Maybe we can handle this with a separate gui/submission sync mutex?
		std::lock_guard devLock(dev.mutex);

		addSubmissionSyncLocked(submitter);
		if(dev.doFullSync) {
			addFullSyncLocked(submitter);
		} else {
			addGuiSyncLocked(submitter);
		}

		{
			ZoneScopedN("dispatch.QueueSubmit");
			std::lock_guard queueLock(dev.queueMutex);

			if(legacy) {
				auto downgraded = submitter.memScope.alloc<VkSubmitInfo>(submitter.submitInfos.size());
				for(auto i = 0u; i < submitter.submitInfos.size(); ++i) {
					downgraded[i] = downgrade(dev, submitter.memScope,
						submitter.submitInfos[i]);
				}

				res = dev.dispatch.QueueSubmit(queue.handle,
					u32(downgraded.size()),
					downgraded.data(),
					submitter.submFence);
			} else {
				res = dev.dispatch.QueueSubmit2(queue.handle,
					u32(submitter.submitInfos.size()),
					submitter.submitInfos.data(),
					submitter.submFence);
			}
		}

		if(res != VK_SUCCESS) {
			dlg_trace("vkQueueSubmit error: {} ({})", vk::name(res), res);
			if(res == VK_ERROR_DEVICE_LOST) {
				onDeviceLost(dev);
			}

			cleanupOnErrorLocked(submitter);
			return res;
		}

		postProcessLocked(submitter);
		dev.pending.push_back(std::move(submitter.dstBatch));
	}

	return res;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(
		VkQueue                                     vkQueue,
		uint32_t                                    submitCount,
		const VkSubmitInfo*                         pSubmits,
		VkFence                                     fence) {
	auto& queue = getData<Queue>(vkQueue);

	// XXX: plain forward for debugging
	// return queue.dev->dispatch.QueueSubmit(queue.handle,
	// 	submitCount, pSubmits, fence);

	ThreadMemScope tms;
	auto submits2 = tms.alloc<VkSubmitInfo2>(submitCount);
	for(auto i = 0u; i < submitCount; ++i) {
		submits2[i] = upgrade(*queue.dev, tms, pSubmits[i]);
	}

	return doSubmit(queue, submits2, fence, true);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(
		VkQueue                                     vkQueue,
		uint32_t                                    submitCount,
		const VkSubmitInfo2*                        pSubmits,
		VkFence                                     fence) {
	auto& queue = getData<Queue>(vkQueue);
	return doSubmit(queue, {pSubmits, submitCount}, fence, false);
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
			if(res == VK_ERROR_DEVICE_LOST) {
				onDeviceLost(*queue.dev);
			}

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
			if(res == VK_ERROR_DEVICE_LOST) {
				onDeviceLost(dev);
			}

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

VKAPI_ATTR VkResult VKAPI_CALL QueueBindSparse(
		VkQueue                                     vkQueue,
		uint32_t                                    bindInfoCount,
		const VkBindSparseInfo*                     pBindInfo,
		VkFence                                     fence) {
	auto& queue = getData<Queue>(vkQueue);
	auto& dev = *queue.dev;

	QueueSubmitter submitter {};
	init(submitter, queue, SubmissionType::bindSparse, fence);

	process(submitter, {pBindInfo, bindInfoCount});

	VkResult res;

	// Lock order is important here, lock dev mutex before queue mutex.
	// We lock the dev mutex to sync with gui.
	{
		// TODO PERF: locking the dev mutex here is terrible for performance,
		// queueSubmit can take a long time and applications might parallelize
		// around it.
		// Maybe we can handle this with a separate gui/submission sync mutex?
		std::lock_guard devLock(dev.mutex);

		addSubmissionSyncLocked(submitter);
		if(dev.doFullSync) {
			addFullSyncLocked(submitter);
		} else {
			addGuiSyncLocked(submitter);
		}

		{
			ZoneScopedN("dispatch.QueueSubmit");
			std::lock_guard queueLock(dev.queueMutex);
			res = queue.dev->dispatch.QueueBindSparse(queue.handle,
				u32(submitter.bindSparseInfos.size()),
				submitter.bindSparseInfos.data(),
				submitter.submFence);
		}

		if(res != VK_SUCCESS) {
			dlg_trace("vkQueueBindSparse error: {} ({})", vk::name(res), res);
			if(res == VK_ERROR_DEVICE_LOST) {
				onDeviceLost(dev);
			}

			cleanupOnErrorLocked(submitter);
			return res;
		}

		postProcessLocked(submitter);
		dev.pending.push_back(std::move(submitter.dstBatch));
	}

	return res;
}

// ugh this is way too messy, maybe build next pointers into submissions
// already at submission time?
Submission* nextSubmissionOrder(Submission& subm) {
	assertOwned(subm.parent->queue->dev->mutex);

	auto subID = &subm - subm.parent->submissions.data();
	dlg_assert(subID >= 0 && subID < i64(subm.parent->submissions.size()));
	if(subID + 1 < i64(subm.parent->submissions.size())) {
		return &subm.parent->submissions[subID + 1];
	}

	auto& dev = *subm.parent->queue->dev;
	// we rely on dev.pending being sorted by submission order here
	for(auto& pending : dev.pending) {
		if(pending->queue == subm.parent->queue &&
				pending->type == SubmissionType::command &&
				!pending->submissions.empty() &&
				pending->submissions[0].queueSubmitID > subm.queueSubmitID) {
			return &pending->submissions[0];
		}
	}

	return nullptr;
}

void activateLocked(MemoryResource& res, const OpaqueSparseMemoryBind& bind) {
	dlg_assert(res.memory.index() == 1u);
	auto& bindState = std::get<1>(res.memory);

	if(bind.memory) {
		auto [it, success] = bindState.opaqueBinds.insert(bind);
		// NOTE The spec does not explicitly state this but
		//   implicit rebinding seems to be allowed
		if(!success) {
			auto& oldBind = *it;
			if(oldBind.memory) {
				auto count = oldBind.memory->allocations.erase(&oldBind);
				dlg_assert(count == 1u);
			}

			dlg_assertm(
				oldBind.resourceOffset == bind.resourceOffset &&
				oldBind.memSize == bind.memSize,
				"TODO: Partial memory rebind not supported");
			const_cast<OpaqueSparseMemoryBind&>(oldBind) = bind;
		}

		bind.memory->allocations.insert(&*it);
	} else {
		// unbind
		auto resEnd = bind.resourceOffset + bind.memSize;
		auto it = bindState.opaqueBinds.lower_bound(bind);
		while(it != bindState.opaqueBinds.end() &&
				it->resourceOffset < resEnd) {
			dlg_assertm(it->resourceOffset + it->memSize <= resEnd,
				"TODO: partial unbinds not support");
			if(it->memory) {
				it->memory->allocations.erase(&*it);
			}
			it = bindState.opaqueBinds.erase(it);
		}
	}
}

void activateLocked(MemoryResource& res, const ImageSparseMemoryBind& bind) {
	dlg_assert(res.memory.index() == 1u);
	auto& bindState = std::get<1>(res.memory);

	if(bind.memory) {
		auto [it, success] = bindState.imageBinds.insert(bind);
		// NOTE The spec does not explicitly state this but
		//   implicit rebinding seems to be allowed
		if(!success) {
			auto& oldBind = *it;
			if(oldBind.memory) {
				auto count = oldBind.memory->allocations.erase(&oldBind);
				dlg_assert(count == 1u);
			}

			dlg_assert(
				oldBind.subres.arrayLayer == bind.subres.arrayLayer &&
				oldBind.subres.mipLevel == bind.subres.mipLevel &&
				oldBind.subres.aspectMask == bind.subres.aspectMask);
			dlg_assertm(oldBind.offset.x == bind.offset.x &&
				oldBind.offset.y == bind.offset.y &&
				oldBind.offset.z == bind.offset.z,
				"TODO: partial rebind not supported");
			dlg_assertm(oldBind.size.width == bind.size.width &&
				oldBind.size.height == bind.size.height &&
				oldBind.size.depth == bind.size.depth,
				"TODO: partial rebind not supported");
			const_cast<ImageSparseMemoryBind&>(oldBind) = bind;
		}
		bind.memory->allocations.insert(&*it);
	} else {
		// TODO: lower bound and iterate? not sure how
		auto it = bindState.imageBinds.find(bind);
		if(it != bindState.imageBinds.end()) {
			dlg_assert(
				it->subres.arrayLayer == bind.subres.arrayLayer &&
				it->subres.mipLevel == bind.subres.mipLevel &&
				it->subres.aspectMask == bind.subres.aspectMask);
			// we currently don't support partial or multi- unbinding
			dlg_assertm(it->offset.x == bind.offset.x &&
				it->offset.y == bind.offset.y &&
				it->offset.z == bind.offset.z,
				"TODO: partial unbind not supported");
			dlg_assertm(it->size.width == bind.size.width &&
				it->size.height == bind.size.height &&
				it->size.depth == bind.size.depth,
				"TODO: partial unbind not supported");

			if(it->memory) {
				it->memory->allocations.erase(&*it);
			}
			it = bindState.imageBinds.erase(it);
		}
	}
}

void activateLocked(BindSparseSubmission& bindSparse) {
	for(auto& bufBind : bindSparse.buffer) {
		dlg_assert(bufBind.dst);
		auto& res = *bufBind.dst;
		for(auto& [bind, _] : bufBind.binds) {
			activateLocked(res, bind);
		}
	}

	for(auto& imgBind : bindSparse.opaqueImage) {
		dlg_assert(imgBind.dst);
		auto& res = *imgBind.dst;
		for(auto& [bind, _] : imgBind.binds) {
			activateLocked(res, bind);
		}
	}

	for(auto& imgBind : bindSparse.image) {
		dlg_assert(imgBind.dst);
		auto& res = *imgBind.dst;
		for(auto& [bind, _] : imgBind.binds) {
			activateLocked(res, bind);
		}
	}
}

void activateLocked(CommandSubmission& cmdSub) {
	for(auto& scb : cmdSub.cbs) {
		auto recPtr = scb.cb->lastRecordPtrLocked();
		// notify hooks that submission was activated
		if(scb.hook) {
			scb.hook->activate();
		} else {
			dlg_assert(scb.accelStructCopies.empty());
			for(auto& copy : recPtr->accelStructCopies) {
				dlg_assert(copy.src->pendingState);
				copy.dst->pendingState = copy.src->pendingState;
				scb.accelStructCopies.push_back(copy.src->pendingState);
			}
		}
		// store pending layouts
		for(auto& ui : recPtr->used.images) {
			ui.handle->applyLocked(ui.layoutChanges);
		}
	}
}

// Called when we know that the submission is ready for execution on the GPU.
// Before this being called, no command can be executed.
void activateLocked(Submission& subm) {
	dlg_assert(!subm.active);

	[[maybe_unused]] auto& dev = *subm.parent->queue->dev;
	assertOwned(dev.mutex);

	subm.active = true;

	// for(auto& wait : subm.waits) {
	// 	if(wait.semaphore->type == VK_SEMAPHORE_TYPE_BINARY) {
	// 		// can be reset any time now
	// 		wait.semaphore->lowerBound = 0u;
	// 	}
	// }

	if(subm.parent->type == SubmissionType::command) {
		auto& cmdSub = std::get<CommandSubmission>(subm.data);
		activateLocked(cmdSub);
	} else if(subm.parent->type == SubmissionType::bindSparse) {
		auto& bindSub = std::get<BindSparseSubmission>(subm.data);
		activateLocked(bindSub);
	} else {
		dlg_error("unreachable");
	}

	for(auto& signal : subm.signals) {
		if(signal->semaphore->type == VK_SEMAPHORE_TYPE_TIMELINE) {
			updateUpperLocked(*signal->semaphore, signal->value);
		}
	}

	if(subm.parent->type == SubmissionType::command &&
			subm.parent->queue->firstWaiting) {
		dlg_assert(subm.parent->queue->firstWaiting == &subm);

		auto* next = nextSubmissionOrder(subm);
		if(next) {
			dlg_assert(!next->active);

			// TODO: bad recursion here.
			// will explode when an applications has some thousand
			// pending inactive submissions.
			subm.parent->queue->firstWaiting = next;
			checkActivateLocked(*next);
		} else {
			subm.parent->queue->firstWaiting = nullptr;
		}
	}
}

bool checkActivateLocked(Submission& subm) {
	assertOwned(subm.parent->queue->dev->mutex);

	dlg_assert(!subm.active);
	dlg_assert(subm.parent->queue->firstWaiting);

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
	if(subm.parent->type == SubmissionType::command &&
			subm.parent->queue->firstWaiting != &subm) {
		return false;
	}

	auto submActive = true;
	for(auto& wait : subm.waits) {
		if(wait->semaphore->type == VK_SEMAPHORE_TYPE_BINARY) {
			dlg_assert(wait->counterpart);
			if(wait->counterpart->submission &&
					!wait->counterpart->submission->active) {
				submActive = false;
				break;
			}
		} else if(wait->value > wait->semaphore->upperBound) {
			submActive = false;
			break;
		}
	}

	if(!submActive) {
		return false;
	}

	activateLocked(subm);
	return true;
}

BindSparseSubmission::BindSparseSubmission() = default;
BindSparseSubmission::~BindSparseSubmission() = default;

} // namespace vil
