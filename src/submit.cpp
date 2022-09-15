#include <submit.hpp>
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
#include <vk/enumString.hpp>
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

void process(QueueSubmitter& subm, VkSubmitInfo2& si) {
	ZoneScoped;

	auto& dev = *subm.dev;

	auto& dst = subm.dstBatch->submissions.emplace_back();
	dst.parent = subm.dstBatch;

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

	// = commandbuffers =
	auto fwdCbs = subm.memScope.alloc<VkCommandBufferSubmitInfo>(si.commandBufferInfoCount);
	dst.cbs.reserve(si.commandBufferInfoCount);
	for(auto j = 0u; j < si.commandBufferInfoCount; ++j) {
		auto& scb = dst.cbs.emplace_back();
		scb.cb = &getCommandBuffer(si.pCommandBufferInfos[j].commandBuffer);

		fwdCbs[j] = si.pCommandBufferInfos[j];
		fwdCbs[j].commandBuffer = scb.cb->handle();
	}

	si.commandBufferInfoCount = fwdCbs.size();
	si.pCommandBufferInfos = fwdCbs.data();
}

void process(QueueSubmitter& subm, span<const VkSubmitInfo2> infos) {
	auto off = subm.submitInfos.size();
	subm.submitInfos = subm.memScope.alloc<VkSubmitInfo2>(off + infos.size());

	subm.dstBatch->submissions.reserve(infos.size());
	for(auto i = 0u; i < infos.size(); ++i) {
		subm.submitInfos[off + i] = infos[i];
		process(subm, subm.submitInfos[off + i]);
	}

	if(subm.dev->commandHook) {
		// after this, the SubmitInfos might contain our hooked command
		// buffers, we must not use getCommandBuffer anymore!
		subm.dev->commandHook->hook(subm);
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

void cleanupOnError(QueueSubmitter& subm) {
	auto& dev = *subm.dev;
	auto& batch = *subm.dstBatch;

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
					dlg_error("vkQueueSubmit error: {}", vk::name(res));

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

	// add to swapchain
	FrameSubmission* recordBatch = nullptr;
	auto swapchain = subm.dev->swapchainLocked();
	if(swapchain) {
		recordBatch = &swapchain->nextFrameSubmissions.batches.emplace_back();
		recordBatch->queue = subm.queue;
		recordBatch->submissionID = subm.globalSubmitID;
	}

	auto& batch = *subm.dstBatch;

	for(auto& sub : batch.submissions) {
		// don't activate when there already is an inactive submission
		// on this queue.
		bool doActivate = !subm.queue->firstWaiting;

		// process cbs
		for(auto& scb : sub.cbs) {
			auto* cb = scb.cb;
			cb->pending.push_back(&sub);
			auto recPtr = cb->lastRecordPtrLocked();

			if(recordBatch) {
				recordBatch->submissions.push_back(recPtr);
			}

			// store pending layouts
			// TODO: postpone to submission activation!
			for(auto& ui : recPtr->used.images) {
				if(ui.layoutChanged) {
					dlg_assert(
						ui.finalLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
						ui.finalLayout != VK_IMAGE_LAYOUT_PREINITIALIZED);
					ui.handle->pendingLayout = ui.finalLayout;
				}
			}
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
				if(!subm.queue->firstWaiting) {
					subm.queue->firstWaiting = &sub;
				}
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
		}
	}

	if(subm.lastLayerSubmission) {
		subm.queue->lastLayerSubmission = (*subm.lastLayerSubmission)->queueSubmitID;
	}
}

// Returns whether the given submission potentially writes the given
// DeviceHandle (only makes sense for Image and Buffer objects)
bool potentiallyWritesLocked(const Submission& subm, const DeviceHandle& handle) {
	// TODO PERF: consider more information, not every use is potentially writing

	assertOwned(handle.dev->mutex);

	Image* img = nullptr;
	Buffer* buf = nullptr;

	// TODO: implement more general mechanism
	// Need const_cast here since we store non-const pointers in rec.used.
	// The find function won't modify it, it's just an interface quirk.
	auto* toFind = const_cast<DeviceHandle*>(&handle);
	if(handle.objectType == VK_OBJECT_TYPE_IMAGE) {
		img = static_cast<Image*>(toFind);
	} else if(handle.objectType == VK_OBJECT_TYPE_BUFFER) {
		buf = static_cast<Buffer*>(toFind);
	} else {
		dlg_error("unreachable");
	}

	for(auto& [cb, _] : subm.cbs) {
		auto& rec = *cb->lastRecordLocked();

		if(buf) {
			auto it = find(rec.used.buffers, *buf);
			if(it != rec.used.buffers.end()) {
				return true;
			}
		} else if(handle.objectType == VK_OBJECT_TYPE_IMAGE) {
			auto it = find(rec.used.images, *img);
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
			if(hasBound(state, handle)) {
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
		for(auto* handle : draw.usedHandles) {
			if(potentiallyWritesLocked(subm, *handle)) {
				subs.push_back(&subm);
				added = true;
				break;
			}
		}

		if(added) {
			continue;
		}

		for(auto& [_, hookPtr] : subm.cbs) {
			// TODO(perf): we might not need sync in all cases. Pretty much only
			// for images and xfb buffers I guess.
			if(hookPtr && hookPtr->record->state.get() == draw.usedHookState.get()) {
				dlg_assert(hookPtr->record->writer == &subm);
				subs.push_back(&subm);
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
