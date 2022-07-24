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

VkCommandBuffer processCB(QueueSubmitter& subm, Submission& dst, VkCommandBuffer vkcb) {
	ZoneScoped;

	auto& dev = *subm.dev;

	auto& cb = getCommandBuffer(vkcb);
	auto& scb = dst.cbs.emplace_back();
	scb.cb = &cb;

	{
		std::lock_guard lock(dev.mutex);
		dlg_assert(cb.state() == CommandBuffer::State::executable);

		auto& rec = *cb.lastRecordLocked();
		dlg_assert(subm.queue->family == rec.queueFamily);

		// potentially hook command buffer
		if(dev.commandHook) {
			auto hooked = dev.commandHook->hook(cb, dst, scb.hook);
			dlg_assert(hooked);
			if(hooked != cb.handle()) {
				subm.lastLayerSubmission = dst.queueSubmitID;
			}

			return hooked;
		} else {
			return cb.handle();
		}
	}
}

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

void process(QueueSubmitter& subm, VkSubmitInfo& si) {
	ZoneScoped;

	auto& dev = *subm.dev;

	auto& dst = subm.dstBatch->submissions.emplace_back();
	dst.parent = subm.dstBatch;

	// id
	{
		auto lock = std::lock_guard(dev.mutex);
		dst.queueSubmitID = ++subm.queue->submissionCounter;
	}

	for(auto j = 0u; j < si.signalSemaphoreCount; ++j) {
		auto& sem = dev.semaphores.get(si.pSignalSemaphores[j]);
		dst.signalSemaphores.push_back(&sem);
	}

	auto cbs = subm.memScope.alloc<VkCommandBuffer>(si.commandBufferCount);
	for(auto j = 0u; j < si.commandBufferCount; ++j) {
		cbs[j] = processCB(subm, dst, si.pCommandBuffers[j]);
	}

	// For wait & present semaphores: if we have timeline semaphores
	// and application added a timeline semaphore submission info to
	// pNext, we have to hook that instead of adding our own.
	VkTimelineSemaphoreSubmitInfo* tsInfo = nullptr;
	if(dev.timelineSemaphores) {
		if(hasChain(si, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)) {
			si.pNext = copyChainLocal(subm.memScope, si.pNext);
			tsInfo = const_cast<VkTimelineSemaphoreSubmitInfo*>(findChainInfo<
				VkTimelineSemaphoreSubmitInfo,
				VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO>(si));
			dlg_assert(tsInfo);
		} else {
			tsInfo = subm.memScope.allocRaw<VkTimelineSemaphoreSubmitInfo>();
			*tsInfo = {};
			tsInfo->sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
			tsInfo->pNext = si.pNext;
			si.pNext = tsInfo;
		}
	}

	// = wait semaphores =
	for(auto j = 0u; j < si.waitSemaphoreCount; ++j) {
		auto& semaphore = dev.semaphores.get(si.pWaitSemaphores[j]);
		dst.waitSemaphores.emplace_back(&semaphore, si.pWaitDstStageMask[j]);
	}

	// = signal semaphores =
	auto signalSems = subm.memScope.alloc<VkSemaphore>(si.signalSemaphoreCount + 1);
	std::copy_n(si.pSignalSemaphores, si.signalSemaphoreCount, signalSems.begin());

	dlg_assert(bool(tsInfo) == dev.timelineSemaphores);
	if(tsInfo) {
		// tsInfo->signalSemaphoreValueCount may be smaller than si.signalSemaphoreCount
		// but then values in between are simply ignored.
		dlg_assert(tsInfo->signalSemaphoreValueCount <= si.signalSemaphoreCount);
		auto signalVals = subm.memScope.alloc<u64>(si.signalSemaphoreCount + 1);
		std::copy_n(tsInfo->pSignalSemaphoreValues, tsInfo->signalSemaphoreValueCount,
			signalVals.begin());

		signalSems.back() = subm.queue->submissionSemaphore;
		signalVals.back() = dst.queueSubmitID;

		dlg_assert(signalVals.size() == signalSems.size());
		tsInfo->signalSemaphoreValueCount = u32(signalVals.size());
		tsInfo->pSignalSemaphoreValues = signalVals.data();
	} else {
		// We need to add a semaphore for device synchronization.
		// We might wanna read from resources that are potentially written
		// by this submission in the future, we need to be able to gpu-sync them.
		dst.ourSemaphore = getSemaphoreFromPool(dev);
		signalSems.back() = dst.ourSemaphore;
	}

	si.signalSemaphoreCount = u32(signalSems.size());
	si.pSignalSemaphores = signalSems.data();

	si.commandBufferCount = u32(cbs.size());
	si.pCommandBuffers = cbs.data();
}

void process(QueueSubmitter& subm, span<const VkSubmitInfo> infos) {
	auto off = subm.submitInfos.size();
	subm.submitInfos = subm.memScope.alloc<VkSubmitInfo>(off + infos.size());

	for(auto i = 0u; i < infos.size(); ++i) {
		subm.submitInfos[off + i] = infos[i];
		process(subm, subm.submitInfos[off + i]);
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

	auto maxWaitSemCount = dev.queues.size();
	auto waitSems = subm.memScope.alloc<VkSemaphore>(maxWaitSemCount);
	auto waitStages = subm.memScope.alloc<VkPipelineStageFlags>(maxWaitSemCount);
	auto waitVals = subm.memScope.alloc<u64>(maxWaitSemCount);
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
			waitVals[waitSemCount] = queue.submissionCounter;
		} else {
			// here we only need to sync with previous layer submissions
			// since we didn't insert any commands ourselves
			if(queue.lastLayerSubmission < finishedID) {
				continue;
			}

			waitVals[waitSemCount] = queue.lastLayerSubmission;
		}

		waitSems[waitSemCount] = queue.submissionSemaphore;
		waitStages[waitSemCount] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		++waitSemCount;
	}

	if(waitSemCount == 0u) {
		return;
	}

	// At this point we know that an additional submission is needed.
	// add it as *first* submission. Need to completely reallocate
	// span for that.
	auto newInfos = subm.memScope.alloc<VkSubmitInfo>(subm.submitInfos.size() + 1);
	std::copy(subm.submitInfos.begin(), subm.submitInfos.end(), newInfos.begin() + 1);
	subm.submitInfos = newInfos;

	auto& si = subm.submitInfos[0];
	si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.pNext = nullptr;
	si.waitSemaphoreCount = waitSemCount;
	si.pWaitSemaphores = waitSems.data();
	si.pWaitDstStageMask = waitStages.data();

	auto& tsInfo = *subm.memScope.allocRaw<VkTimelineSemaphoreSubmitInfo>();
	tsInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
	tsInfo.pNext = NULL;

	tsInfo.waitSemaphoreValueCount = u32(waitVals.size());
	tsInfo.pWaitSemaphoreValues = waitVals.data();

	si.pNext = &tsInfo;
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
	if(dev.gui && dev.gfxQueue != subm.queue) {
		// since all draws are submitted to the same queue
		// we only need to wait upon the last one pending.
		insertGuiSync = dev.gui->latestPendingDrawSyncLocked(batch);
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
	auto newInfos = subm.memScope.alloc<VkSubmitInfo>(subm.submitInfos.size() + 1);
	std::copy(subm.submitInfos.begin(), subm.submitInfos.end(), newInfos.begin() + 1);
	subm.submitInfos = newInfos;

	auto& si = subm.submitInfos[0];
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.pNext = nullptr;

	auto maxWaitSemCount = dev.resetSemaphores.size() + 1;
	auto waitSems = subm.memScope.alloc<VkSemaphore>(maxWaitSemCount);
	auto waitStages = subm.memScope.alloc<VkPipelineStageFlags>(maxWaitSemCount);

	std::copy(dev.resetSemaphores.begin(), dev.resetSemaphores.end(), waitSems.begin());
	std::fill_n(waitStages.begin(), dev.resetSemaphores.size(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	auto waitSemCount = dev.resetSemaphores.size();

	if(insertGuiSync) {
		auto& waitDraw = *insertGuiSync;
		dlg_assert(waitDraw.futureSemaphoreSignaled);

		// PERF(sync): when there are multiple submissions in the batch we could
		// sync on a per-submission basis (instead of per-batch). This is only
		// practical for timeline semaphores I guess since otherwise we can't
		// use the src semaphore multiple times.
		if(dev.timelineSemaphores) {
			auto& tsInfo = *subm.memScope.allocRaw<VkTimelineSemaphoreSubmitInfo>();
			tsInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
			tsInfo.pNext = NULL;

			auto waitVals = subm.memScope.alloc<u64>(1);
			waitSems[waitSemCount] = dev.gui->usedQueue().submissionSemaphore;
			waitVals[waitSemCount] = waitDraw.lastSubmissionID;
			waitStages[waitSemCount] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
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

				waitSems[waitSemCount] = guiSyncSemaphore;
				waitStages[waitSemCount] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
				++waitSemCount;

				batch.poolSemaphores.push_back(guiSyncSemaphore);
			} else {
				waitDraw.futureSemaphoreUsed = true;
				waitSems[waitSemCount] = waitDraw.futureSemaphore;
				waitStages[waitSemCount] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
				// DON'T add waitDraw.futureSemaphore to batch.poolSemaphores
				// here since it's owned by the gui.

				subm.syncedGuiDraw = &waitDraw;
			}
		}
	}

	batch.poolSemaphores = std::move(dev.resetSemaphores);
	si.waitSemaphoreCount = waitSemCount;
	si.pWaitSemaphores = waitSems.data();
	si.pWaitDstStageMask = waitStages.data();
}

void postProcessLocked(QueueSubmitter& subm) {
	ZoneScoped;

	FrameSubmission* recordBatch = nullptr;
	if(subm.dev->swapchain) {
		recordBatch = &subm.dev->swapchain->nextFrameSubmissions.batches.emplace_back();
		recordBatch->queue = subm.queue;
		recordBatch->submissionID = subm.globalSubmitID;
	}

	auto& batch = *subm.dstBatch;

	for(auto& sub : batch.submissions) {
		for(auto& scb : sub.cbs) {
			auto* cb = scb.cb;
			cb->pending.push_back(&sub);
			auto recPtr = cb->lastRecordPtrLocked();

			if(recordBatch) {
				recordBatch->submissions.push_back(recPtr);
			}

			// store pending layouts
			for(auto& used : recPtr->handles) {
				if(used.first->objectType != VK_OBJECT_TYPE_IMAGE) {
					continue;
				}

				auto& img = static_cast<Image&>(*used.first);
				auto& ui = static_cast<UsedImage&>(*used.second);
				if(ui.layoutChanged) {
					dlg_assert(
						ui.finalLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
						ui.finalLayout != VK_IMAGE_LAYOUT_PREINITIALIZED);
					img.pendingLayout = ui.finalLayout;
				}
			}
		}
	}

	if(subm.lastLayerSubmission) {
		subm.queue->lastLayerSubmission = *subm.lastLayerSubmission;
	}
}

// TODO WIP
// bool potentiallyWrites(const Command& bcmd, const Buffer& buf) {
// 	if(typeid(bcmd) == typeid(CopyBufferCmd)) {
// 		auto& cmd = static_cast<const CopyBufferCmd&>(bcmd);
// 		dlg_assert(cmd.src == &buf || cmd.dst == &buf);
// 		return cmd.dst == &buf;
// 	} else if(typeid(bcmd) == typeid(CopyBufferToImageCmd)) {
// 	}
// }
//
// bool potentiallyWritesLocked(const CommandRecord& rec, const Buffer& buf) {
// 	auto it = rec.handles.find(handleToU64(buf.handle));
// 	if(it != rec.handles.end()) {
// 		auto& uh = it->second;
// 		for(auto* cmd : uh.commands) {
// 		}
// 	}
// }

// Returns whether the given submission potentially writes the given
// DeviceHandle (only makes sense for Image and Buffer objects)
bool potentiallyWritesLocked(const Submission& subm, const DeviceHandle& handle) {
	// TODO(perf): we only need to do this if a record
	// can potentially write the handle. Could track
	// during recording and check below in ds refs.

	assertOwned(handle.dev->mutex);

	const Image* img = nullptr;
	const Buffer* buf = nullptr;

	if(handle.objectType == VK_OBJECT_TYPE_IMAGE) {
		img = static_cast<const Image*>(&handle);
	} else if(handle.objectType == VK_OBJECT_TYPE_BUFFER) {
		buf = static_cast<const Buffer*>(&handle);
	}

	for(auto& [cb, _] : subm.cbs) {
		auto& rec = *cb->lastRecordLocked();

		// Need const_cast here since we store non-const pointers in rec.handles.
		// The find function won't modify it, it's just an interface quirk.
		auto* toFind = const_cast<DeviceHandle*>(&handle);
		if(rec.handles.find(toFind) != rec.handles.end()) {
			return true;
		}

		for(auto& pair : rec.handles) {
			dlg_assert(pair.first);

			auto& rhandle = *pair.first;
			if(rhandle.objectType != VK_OBJECT_TYPE_DESCRIPTOR_SET) {
				continue;
			}

			// important that the ds mutex is locked mainly for
			// update_unused_while_pending.
			auto& state = static_cast<DescriptorSet&>(rhandle);
			auto lock = state.lock();

			for(auto& binding : state.layout->bindings) {
				auto dsCat = DescriptorCategory(binding.descriptorType);
				if(img && dsCat == DescriptorCategory::image) {
					for(auto& elem : images(state, binding.binding)) {
						if(elem.imageView && elem.imageView->img == img) {
							return true;
						}
					}
				} else if(buf && dsCat == DescriptorCategory::buffer) {
					for(auto& elem : buffers(state, binding.binding)) {
						if(elem.buffer && elem.buffer == buf) {
							return true;
						}
					}
				}

				// NOTE: we don't have to care for buffer views as they
				// are always readonly.
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

} // vil
