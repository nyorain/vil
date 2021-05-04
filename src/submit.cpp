#include <submit.hpp>
#include <device.hpp>
#include <queue.hpp>
#include <image.hpp>
#include <buffer.hpp>
#include <ds.hpp>
#include <sync.hpp>
#include <cb.hpp>
#include <swapchain.hpp>
#include <util/util.hpp>
#include <util/intrusive.hpp>
#include <gui/commandHook.hpp>
#include <gui/gui.hpp>
#include <vk/enumString.hpp>
#include <tracy/Tracy.hpp>

namespace vil {

void processCB(QueueSubmitter& subm, Submission& dst, VkCommandBuffer vkcb) {
	ZoneScoped;

	auto& dev = *subm.dev;

	auto& cb = dev.commandBuffers.get(vkcb);
	auto& scb = dst.cbs.emplace_back();
	scb.cb = &cb;

	{
		std::lock_guard lock(dev.mutex);
		dlg_assert(cb.state() == CommandBuffer::State::executable);

		auto& rec = *cb.lastRecordLocked();
		dlg_assert(subm.queue->family == rec.queueFamily);

		// update the descriptor state of the record
		// TODO, PERF: we only need to do this when update_after_bind
		// descriptors in the record changed since the last submission
		rec.lastDescriptorState.states.clear();
		for(auto& pair : rec.handles) {
			auto& handle = nonNull(pair.second.handle);
			if(handle.objectType == VK_OBJECT_TYPE_DESCRIPTOR_SET) {
				auto& ds = static_cast<const DescriptorSet&>(handle);
				rec.lastDescriptorState.states[static_cast<void*>(&handle)] = ds.state;
			}
		}

		// find submission group to check for hook
		// When the CommandRecord doesn't already have a group (from
		// previous submission), try to find an existing group
		// that matches this submission.
		float best = -1.0;
		if(!rec.group) {
			auto& qfam = dev.queueFamilies[subm.queue->family];
			for(auto& qgroup : qfam.commandGroups) {
				// TODO: make configurable? This determines how
				// much we allow a CommandRecord to differ from the groups
				// description while still recognizing it as group member.
				constexpr auto matchThreshold = 0.8;
				auto matchVal = match(qgroup->desc, rec.desc);
				if(matchVal > best && matchVal > matchThreshold) {
					rec.group = qgroup.get();
					best = matchVal;
				}
			}

			if(rec.group) {
				rec.group->aliveRecords.insert(&rec);
			}
		}

		// When no existing group matches, just create a new one
		if(!rec.group) {
			auto& qfam = dev.queueFamilies[rec.queueFamily];
			rec.group = qfam.commandGroups.emplace_back(std::make_unique<CommandBufferGroup>()).get();
			subm.queue->groups.insert(rec.group);
			rec.group->queues = {{subm.queue, subm.submitID}};
			rec.group->aliveRecords.insert(&rec);
		} else {
			// Otherwise make sure it's correctly added to the group
			// NOTE: we even do this if submission fails down the
			// chain. Should not be a problem really
			auto finder = [&](auto& q) { return q.first == subm.queue; };
			auto it = find_if(rec.group->queues, finder);
			if(it == rec.group->queues.end()) {
				rec.group->queues.push_back({subm.queue, subm.submitID});
				subm.queue->groups.insert(rec.group);
			} else {
				it->second = subm.submitID;
			}

			dlg_assert(contains(rec.group->aliveRecords, &rec));
		}

		dlg_assert(rec.group);

		// potentially hook command buffer
		if(dev.commandHook) {
			auto hooked = dev.commandHook->hook(cb, dst, scb.hook);
			dlg_assert(hooked);
			subm.commandBuffers.back().push_back(hooked);
		} else {
			subm.commandBuffers.back().push_back(cb.handle());
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

	VkSemaphoreTypeCreateInfo tsci {}; // keep-alive
	if(dev.timelineSemaphores) {
		tsci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
		tsci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		tsci.initialValue = 0u;
		sci.pNext = &tsci;
	}

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

void process(QueueSubmitter& subm, const VkSubmitInfo& info) {
	ZoneScoped;

	auto& dev = *subm.dev;

	auto si = info;
	auto& dst = subm.dstBatch->submissions.emplace_back();
	dst.parent = subm.dstBatch;

	for(auto j = 0u; j < si.signalSemaphoreCount; ++j) {
		auto& sem = dev.semaphores.get(si.pSignalSemaphores[j]);
		dst.signalSemaphores.push_back(&sem);

		// std::lock_guard lock(dev.mutex);
		// dlg_assert(!sem.signalFrom);
		// dlg_assert(!sem.waitFrom);
		// sem.signalFrom = &subm;
	}

	auto& cbs = subm.commandBuffers.emplace_back();
	for(auto j = 0u; j < si.commandBufferCount; ++j) {
		processCB(subm, dst, si.pCommandBuffers[j]);
	}

	// For wait & present semaphores: if we have timeline semaphores
	// and application added a timeline semaphore submission info to
	// pNext, we have to hook that instead of adding our own.
	VkTimelineSemaphoreSubmitInfo* tsInfo = nullptr;
	if(dev.timelineSemaphores) {
		if(hasChain(si, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)) {
			copyChain(si.pNext, subm.copiedChains.emplace_back());
			tsInfo = const_cast<VkTimelineSemaphoreSubmitInfo*>(findChainInfo<
				VkTimelineSemaphoreSubmitInfo,
				VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO>(si));
			dlg_assert(tsInfo);
		} else {
			tsInfo = &subm.tsSubmitInfos.emplace_back();
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

		// std::lock_guard lock(dev.mutex);
		// dlg_assert(!semaphore.waitFrom);
		// dlg_assert(semaphore.signalFrom);
		// semaphore.waitFrom = &subm;
	}

	// = signal semaphores =
	// We need to add a semaphore for device synchronization.
	// We might wanna read from resources that are potentially written
	// by this submission in the future, we need to be able to gpu-sync them.
	dst.ourSemaphore = getSemaphoreFromPool(dev);

	auto& signalSems = subm.semaphores.emplace_back(
		si.pSignalSemaphores,
		si.pSignalSemaphores + si.signalSemaphoreCount);
	signalSems.push_back(dst.ourSemaphore);

	dlg_assert(bool(tsInfo) == dev.timelineSemaphores);
	if(tsInfo) {
		u64 ourValue;
		VK_CHECK(dev.dispatch.GetSemaphoreCounterValue(dev.handle, dst.ourSemaphore, &ourValue));
		++ourValue;

		dst.ourSemaphoreValue = ourValue;

		auto& signalVals = subm.tsValues.emplace_back(
			tsInfo->pSignalSemaphoreValues,
			tsInfo->pSignalSemaphoreValues + tsInfo->signalSemaphoreValueCount);
		signalVals.resize(si.signalSemaphoreCount); // ignored
		signalVals.push_back(ourValue);

		dlg_assert(signalVals.size() == signalSems.size());
		tsInfo->signalSemaphoreValueCount = u32(signalVals.size());
		tsInfo->pSignalSemaphoreValues = signalVals.data();
	}

	si.signalSemaphoreCount = u32(signalSems.size());
	si.pSignalSemaphores = signalSems.data();

	si.commandBufferCount = u32(cbs.size());
	si.pCommandBuffers = cbs.data();

	subm.submitInfos.push_back(si);
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

VkResult addGuiSyncLocked(QueueSubmitter& subm) {
	ZoneScoped;

	auto& dev = *subm.dev;
	auto& batch = *subm.dstBatch;

	// When this submission uses the gfxQueue (which we also use for
	// gui rendering) we don't have to synchronize with gui rendering
	// via semaphores, the pipeline barrier is enough.
	if(!dev.gui || (dev.gfxQueue == subm.queue && !forceGfxQueueSemaphores)) {
		return VK_SUCCESS;
	}

	// since all draws are submitted to the same queue
	// we only need to wait upon the last one pending.
	auto* pWaitDraw = dev.gui->latestPendingDrawSyncLocked(batch);
	if(!pWaitDraw) {
		return VK_SUCCESS;
	}

	auto& waitDraw = *pWaitDraw;
	dlg_assert(waitDraw.futureSemaphoreSignaled);

	// add as *first* submission
	auto& si = *subm.submitInfos.emplace(subm.submitInfos.begin());

	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.pNext = nullptr;

	auto& waitSems = subm.semaphores.emplace_back();
	auto& waitStages = subm.waitStages.emplace_back();

	// PERF(sync): when there are multiple submissions in the batch we could
	// sync on a per-submission basis (instead of per-batch). This is only
	// practical for timeline semaphores I guess since otherwise we can't
	// use the src semaphore multiple times.
	if(dev.timelineSemaphores) {
		auto& tsInfo = subm.tsSubmitInfos.emplace_back();
		tsInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		tsInfo.pNext = NULL;

		waitSems.push_back(waitDraw.futureSemaphore);
		waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

		auto& waitVals = subm.tsValues.emplace_back();
		waitVals.push_back(waitDraw.futureSemaphoreValue);

		tsInfo.waitSemaphoreValueCount = u32(waitVals.size());
		tsInfo.pWaitSemaphoreValues = waitVals.data();

		si.pNext = &tsInfo;
	} else {
		// use this to reset all semaphores
		waitSems = std::move(dev.resetSemaphores);
		waitStages.resize(waitSems.size(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

		batch.poolSemaphores = waitSems;

		if(waitDraw.futureSemaphoreUsed) {
			// we don't have timeline semaphores and have already
			// chained the future semaphore of that draw somewhere, ugh.
			// Add a new semaphore to the graphics queue.
			auto guiSyncSemaphore = getSemaphoreFromPoolLocked(dev);
			auto res = submitSemaphore(*subm.queue, guiSyncSemaphore);

			if(res != VK_SUCCESS) {
				dlg_trace("vkQueueSubmit error: {}", vk::name(res));
				cleanupOnError(subm);
				return res;
			}

			waitSems.push_back(guiSyncSemaphore);
			waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
			batch.poolSemaphores.push_back(guiSyncSemaphore);
		} else {
			waitDraw.futureSemaphoreUsed = true;
			waitSems.push_back(waitDraw.futureSemaphore);
			waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
			// DON'T add waitDraw.futureSemaphore to batch.poolSemaphores
			// here since it's owned by the gui.

			subm.syncedGuiDraw = &waitDraw;
		}
	}

	si.waitSemaphoreCount = u32(waitSems.size());
	si.pWaitSemaphores = waitSems.data();
	si.pWaitDstStageMask = waitStages.data();

	return VK_SUCCESS;
}

std::vector<std::unique_ptr<CommandBufferGroup>> cleanCommandGroups(Device& dev, u32 qfam) {
	ZoneScoped;
	std::vector<std::unique_ptr<CommandBufferGroup>> ret;

	// Remove old command groups, making sure we don't just leak them.
	auto& qf = dev.queueFamilies[qfam];
	for(auto it = qf.commandGroups.begin(); it != qf.commandGroups.end();) {
		auto rem = true;

		auto& group = **it;

		// When there is still a record of this group kept alive
		// elsewhere don't destroy the group. It is either in use by some
		// gui/high-level component or still valid in command buffer.
		// When the application is keeping a record alive, the chances are
		// high it will be used again. Since gui and application can only
		// keep a limited number of records alive, this should not result
		// in an effective leak.
		if(group.aliveRecords.size() > 1 || group.lastRecord->refCount.load() > 1) {
			++it;
			continue;
		}

		// Otherwise, remove the group if there have been a lot of submission
		// on all its known queues since the last time a cb of the group
		// itself was submitted.
		// NOTE: could alternatively also factor in time. Or the number
		// of existent command groups, i.e. only start this when we have
		// more than X groups already and just throw out the ones not used
		// for the longest time/submissions. Then only do this when
		// a new group is actually added.
		constexpr auto submissionCountThreshold = 10u;
		for(auto& [squeue, lastSubm] : group.queues) {
			if(squeue->submissionCount - lastSubm < submissionCountThreshold) {
				rem = false;
				break;
			}
		}

		if(rem) {
			for(auto& [squeue, _] : group.queues) {
				squeue->groups.erase(it->get());
			}

			ret.push_back(std::move(*it));
			it = qf.commandGroups.erase(it);
		} else {
			++it;
		}
	}

	return ret;
}

void postProcessLocked(QueueSubmitter& subm) {
	ZoneScoped;

	RecordBatch* recordBatch = nullptr;
	if(subm.dev->swapchain) {
		recordBatch = &subm.dev->swapchain->nextFrameSubmissions.emplace_back();
		recordBatch->queue = subm.queue;
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
			for(auto& used : recPtr->images) {
				if(used.second.layoutChanged) {
					dlg_assert(
						used.second.finalLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
						used.second.finalLayout != VK_IMAGE_LAYOUT_PREINITIALIZED);
					used.second.image->pendingLayout = used.second.finalLayout;
				}
			}

			// we already set the command group before, look above
			dlg_assert(recPtr->group);

			if(recPtr->group->lastRecord) {
				subm.keepAliveRecs.push_back(std::move(recPtr->group->lastRecord));
			}

			recPtr->group->lastRecord = recPtr;
			// NOTE: not 100% sure about this in case of already existent group.
			// Do we continuously want to update the description?
			// It sounds like a good idea though, allowing small changes
			// from submission to submission, e.g. when camera is
			// looking around.
			recPtr->group->desc = recPtr->desc;
		}
	}

	subm.keepAliveGroups = cleanCommandGroups(*subm.dev, subm.queue->family);
}

// Returns whether the given submission potentially writes the given
// DeviceHandle (only makes sense for Image and Buffer objects)
bool potentiallyWritesLocked(const Submission& subm, const DeviceHandle& handle) {
	// TODO(perf): we only need to do this if a record
	// can potentially write the handle. Could track
	// during recording and check below in ds refs.

	dlg_assert(handle.dev->mutex.owned());

	const Image* img = nullptr;
	const Buffer* buf = nullptr;

	if(handle.objectType == VK_OBJECT_TYPE_IMAGE) {
		img = static_cast<const Image*>(&handle);
	} else if(handle.objectType == VK_OBJECT_TYPE_BUFFER) {
		buf = static_cast<const Buffer*>(&handle);
	}

	for(auto& [cb, _] : subm.cbs) {
		auto& rec = *cb->lastRecordLocked();
		if(handle.refRecords.find(&rec) != handle.refRecords.end()) {
			return true;
			break;
		}

		if(img) {
			for(auto* view : img->views) {
				for(auto& ds : view->descriptors) {
					dlg_assert(ds.state);
					if(!ds.state->ds) {
						continue;
					}

					// TODO: could check here if it is bound as writeable image
					if(ds.state->ds->refRecords.find(&rec) != ds.state->ds->refRecords.end()) {
						return true;
					}
				}
			}
		}

		if(buf) {
			for(auto& ds : buf->descriptors) {
				dlg_assert(ds.state);
				if(!ds.state->ds) {
					continue;
				}

				// TODO: could check here if it is bound as writeable buffer
				if(ds.state->ds->refRecords.find(&rec) != ds.state->ds->refRecords.end()) {
					return true;
				}
			}

			// NOTE: we don't have to care for buffer views as they
			// are always readonly.
		}
	}

	return false;
}

std::vector<const Submission*> needsSyncLocked(const SubmissionBatch& pending, const Draw& draw) {
	ZoneScoped;

	auto& dev = *pending.queue->dev;
	if(pending.queue == dev.gfxQueue && !forceGfxQueueSemaphores) {
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
				dlg_assert(draw.usedHookState->writer == &subm);
				subs.push_back(&subm);
			}
		}
	}

	return subs;
}

} // vil
