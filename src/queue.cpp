#include <queue.hpp>
#include <data.hpp>
#include <cb.hpp>
#include <ds.hpp>
#include <commands.hpp>
#include <swapchain.hpp>
#include <sync.hpp>
#include <buffer.hpp>
#include <image.hpp>
#include <deque>
#include <gui/gui.hpp>
#include <gui/commandHook.hpp>
#include <util/util.hpp>
#include <vk/enumString.hpp>

namespace vil {

std::optional<SubmIterator> checkLocked(SubmissionBatch& subm) {
	auto& dev = *subm.queue->dev;

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

			auto it2 = std::find(cb->pending.begin(), cb->pending.end(), &subm);
			dlg_assert(it2 != cb->pending.end());
			cb->pending.erase(it2);
		}

		// TODO: submission rework/display
		// for(auto& [sem, stage] : sub.waitSemaphores) {
			// dlg_assert(sem->waitFrom == &subm);
			// sem->waitFrom = nullptr;
		// }

		// for(auto* sem : sub.signalSemaphores) {
			// dlg_assert(sem->signalFrom == &subm);
			// sem->signalFrom = nullptr;
		// }

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

	return dev.pending.erase(it);
}

SubmittedCommandBuffer::SubmittedCommandBuffer() = default;
SubmittedCommandBuffer::~SubmittedCommandBuffer() = default;

CommandBufferGroup::CommandBufferGroup() = default;
CommandBufferGroup::~CommandBufferGroup() = default;

VkFence getFenceFromPool(Device& dev, bool& checkedSubmissions) {
	{
		std::lock_guard lock(dev.mutex);
		if(!dev.fencePool.empty()) {
			auto ret = dev.fencePool.back();
			dev.fencePool.pop_back();
			return ret;
		}

		// check if a submission finished
		if(!checkedSubmissions) {
			// we can iterate through it like that (even though checkLocked removes)
			// since we return on the first return.
			for(auto it = dev.pending.begin(); it < dev.pending.end(); ++it) {
				auto& subm = *it;
				if(subm->ourFence && checkLocked(*subm)) {
					dlg_assert(!dev.fencePool.empty());
					auto ret = dev.fencePool.back();
					dev.fencePool.pop_back();
					return ret;
				}
			}

			checkedSubmissions = true;
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

VkSemaphore getSemaphoreFromPool(Device& dev, bool& checkedSubmissions,
		bool& resettedSemaphores, VkQueue resetQueue) {
	std::unique_lock lock(dev.mutex);
	if(!dev.semaphorePool.empty()) {
		auto ret = dev.semaphorePool.back();
		dev.semaphorePool.pop_back();
		return ret;
	}

	// If there are enough semaphores in the reset pool, it's worth
	// resetting it.
	// NOTE: this is somewhat ugly. But without it, we might create a
	// shitload of semaphore when the application submits
	// a lot of command buffers without us ever getting another chance
	// to reset them. We don't need this ugliness with timeline semaphores anyways.
	dlg_assert(dev.resetSemaphores.empty() || !dev.timelineSemaphores);
	constexpr auto minResetSemCount = 10u;
	if(!resettedSemaphores && dev.resetSemaphores.size() > minResetSemCount) {
		if(!checkedSubmissions) {
			// first check whether any additional pending submissions have finished.
			for(auto it = dev.pending.begin(); it != dev.pending.end();) {
				auto& subm = *it;
				auto nit = checkLocked(*subm);
				it = nit ? *nit : it + 1;
			}

			checkedSubmissions = true;
		}

		auto resetSemaphores = std::move(dev.resetSemaphores);
		lock.unlock();

		auto waitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		std::vector<VkPipelineStageFlags> waitFlags(resetSemaphores.size(), waitStage);

		VkSubmitInfo si {};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.waitSemaphoreCount = u32(resetSemaphores.size());
		si.pWaitSemaphores = resetSemaphores.data();
		si.pWaitDstStageMask = waitFlags.data();

		auto fence = getFenceFromPool(dev, checkedSubmissions);

		{
			std::lock_guard queueLock(dev.queueMutex);
			VK_CHECK(dev.dispatch.QueueSubmit(resetQueue, 1, &si, fence));
		}

		VK_CHECK(dev.dispatch.WaitForFences(dev.handle, 1, &fence, true, UINT64_MAX));

		auto ret = resetSemaphores.back();
		resetSemaphores.pop_back();
		VK_CHECK(dev.dispatch.ResetFences(dev.handle, 1, &fence));

		lock.lock();
		dev.fencePool.push_back(fence);

		dev.semaphorePool.insert(dev.semaphorePool.end(),
			resetSemaphores.begin(), resetSemaphores.end());

		resettedSemaphores = true;
		return ret;
	}

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

void print(const CommandBufferDesc& desc, unsigned indent = 0u) {
	std::string is;
	is.resize(indent, ' ');

	dlg_trace("{}{} {}", is, desc.name, desc.totalCommands);
	dlg_trace("{} dispatch: {}", is, desc.dispatchCommands);
	dlg_trace("{} draw: {}", is, desc.drawCommands);
	dlg_trace("{} transfer: {}", is, desc.transferCommands);
	dlg_trace("{} sync: {}", is, desc.syncCommands);
	dlg_trace("{} query: {}", is, desc.queryCommands);

	for(auto& child : desc.children) {
		print(child, indent + 1);
	}
}

// Returns whether the given submission potentially writes the given
// DeviceHandle.
bool potentiallyWritesLocked(Submission& subm, DeviceHandle& handle) {
	// TODO(perf): we only need to do this if a record
	// can potentially write the handle. Could track
	// during recording and check below in ds refs.

	Image* img = nullptr;
	Buffer* buf = nullptr;

	if(handle.objectType == VK_OBJECT_TYPE_IMAGE) {
		img = static_cast<Image*>(&handle);
	} else if(handle.objectType == VK_OBJECT_TYPE_BUFFER) {
		buf = static_cast<Buffer*>(&handle);
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
					// TODO: could check here if it is bound as writeable image
					if(ds.ds->refRecords.find(&rec) != ds.ds->refRecords.end()) {
						return true;
					}
				}
			}
		}

		if(buf) {
			for(auto& ds : buf->descriptors) {
				// TODO: could check here if it is bound as writeable buffer
				if(ds.ds->refRecords.find(&rec) != ds.ds->refRecords.end()) {
					return true;
				}
			}

			// NOTE: we don't have to care for buffer views as they
			// are always readonly.
		}
	}

	return false;
}

std::unordered_set<Submission*> needsSyncLocked(SubmissionBatch& pending, Draw& draw) {
	auto& dev = *pending.queue->dev;
	if(pending.queue == dev.gfxQueue) {
		return {};
	}

	std::unordered_set<Submission*> subs;
	for(auto& subm : pending.submissions) {
		auto added = false;
		for(auto* handle : draw.usedHandles) {
			if(potentiallyWritesLocked(subm, *handle)) {
				subs.insert(&subm);
				added = true;
				break;
			}
		}

		if(added) {
			continue;
		}

		for(auto& [_, hookPtr] : subm.cbs) {
			// TODO: we might not need sync in all cases. Pretty much only
			// for images I guess
			if(hookPtr && hookPtr->record->state.get() == draw.usedHookState.get()) {
				subs.insert(&subm);
			}
		}
	}

	return subs;
}

// TODO: function is becoming way too big and complicated, create
// SubmissionBuilder class.
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(
		VkQueue                                     queue,
		uint32_t                                    submitCount,
		const VkSubmitInfo*                         pSubmits,
		VkFence                                     fence) {
	auto& qd = getData<Queue>(queue);
	auto& dev = *qd.dev;

	bool checkedSubmissions = false;
	bool resettedSemaphores = false;

	u32 qSubmitID;
	{
		std::lock_guard lock(dev.mutex);
		qSubmitID = ++qd.submissionCount;
	}

	// hook fence
	auto submPtr = std::make_unique<SubmissionBatch>();
	auto& subm = *submPtr;
	subm.submissions.reserve(submitCount); // make sure it's never re-allocated

	subm.queue = &qd;

	// TODO(perf): we make a lot of allocations here and this is something
	// that can be called multiple times per frame. Should likely use
	// an allocator
	// TODO: before even starting to do anything, we should likely check
	// all pending submissions for completion. Might change hooking behavior
	// and we might do it anyways later on to return fences/semaphores to the pools

	// We might have to modify the submission:
	// - command buffers can be hooked (to allow us inserting/removing/changing
	//   commands), meaning we use an internal command buffer as replacement
	// - we add our own semaphore to every submission to allow tracking
	//   when they are finished (we can use that in the gui to use the resources
	//   ourselves without having to wait on cpu for the submissions to complete)
	std::vector<VkSubmitInfo> nsubmitInfos;
	std::vector<std::vector<VkSemaphore>> semaphores;
	std::vector<std::vector<VkPipelineStageFlags>> waitStages;
	std::vector<std::vector<VkCommandBuffer>> commandBuffers;
	std::vector<std::unique_ptr<std::byte[]>> copiedChains;

	std::vector<std::vector<u64>> tsValues;
	std::deque<VkTimelineSemaphoreSubmitInfo> tsSubmitInfos;

	for(auto i = 0u; i < submitCount; ++i) {
		auto si = pSubmits[i]; // copy it
		auto& dst = subm.submissions.emplace_back();
		dst.parent = &subm;

		for(auto j = 0u; j < si.signalSemaphoreCount; ++j) {
			auto& sem = dev.semaphores.get(si.pSignalSemaphores[j]);
			dst.signalSemaphores.push_back(&sem);

			// std::lock_guard lock(dev.mutex);
			// dlg_assert(!sem.signalFrom);
			// dlg_assert(!sem.waitFrom);
			// sem.signalFrom = &subm;
		}

		auto& cbs = commandBuffers.emplace_back();
		for(auto j = 0u; j < si.commandBufferCount; ++j) {
			auto& cb = dev.commandBuffers.get(si.pCommandBuffers[j]);
			auto& scb = dst.cbs.emplace_back();
			scb.cb = &cb;

			{
				std::lock_guard lock(dev.mutex);
				dlg_assert(cb.state() == CommandBuffer::State::executable);

				// find submission group to check for hook
				auto& rec = *cb.lastRecordLocked();
				dlg_assert(qd.family == rec.queueFamily);

				// When the CommandRecord doesn't already have a group (from
				// previous submission), try to find an existing group
				// that matches this submission.
				float best = -1.0;
				if(!rec.group) {
					auto& qfam = dev.queueFamilies[qd.family];
					for(auto& qgroup : qfam.commandGroups) {
						// TODO: make configurable? This determines how
						// much we allow a CommandRecord to differ from the groups
						// description while still recognizing it as group member.
						constexpr auto matchThreshold = 0.6;
						auto matchVal = match(qgroup->desc, rec.desc);
						best = std::max(best, matchVal);
						if(matchVal > matchThreshold) {
							rec.group = qgroup.get();
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
					qd.groups.insert(rec.group);
					rec.group->queues = {{&qd, qSubmitID}};
					rec.group->aliveRecords.insert(&rec);
				} else {
					// Otherwise make sure it's correctly added to the group
					// NOTE: we even do this if submission fails down the
					// chain. Should not be a problem really
					auto finder = [&](auto& q) { return q.first == &qd; };
					auto it = find_if(rec.group->queues, finder);
					if(it == rec.group->queues.end()) {
						rec.group->queues.push_back({&qd, qSubmitID});
						qd.groups.insert(rec.group);
					} else {
						it->second = qSubmitID;
					}
				}

				dlg_assert(rec.group);

				// potentially hook command buffer
				if(dev.commandHook) {
					auto hooked = dev.commandHook->hook(cb, dst, scb.hook);
					dlg_assert(hooked);
					cbs.push_back(hooked);
				} else {
					cbs.push_back(cb.handle());
				}
			}
		}

		// For wait & present semaphores: if we have timeline semaphores
		// and application added a timeline semaphore submission info to
		// pNext, we have to hook that instead of adding our own.
		VkTimelineSemaphoreSubmitInfo* tsInfo = nullptr;
		if(dev.timelineSemaphores) {
			if(hasChain(si, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)) {
				copyChain(si.pNext, copiedChains.emplace_back());
				tsInfo = const_cast<VkTimelineSemaphoreSubmitInfo*>(findChainInfo<
					VkTimelineSemaphoreSubmitInfo,
					VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO>(si));
				dlg_assert(tsInfo);
			} else {
				tsInfo = &tsSubmitInfos.emplace_back();
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

		{
			// TODO: local lock isn't really sufficient here. If gui
			// is destructed after this critical section but before
			// vkQueueSubmit bad things might happen?
			// Figure out sync here!
			std::lock_guard lock(dev.mutex);

			// When there is an active gui that submits to a different queue
			// than this one, we have to make sure this submission waits
			// on draws reading resources this submission writes.
			// When the gfxQueue (that the gui uses) is the same as the
			// one for this submission, the barrier at the end of the gui
			// submission is enough.
			if(dev.gui && dev.gfxQueue != &qd) {
				auto waitDraws = dev.gui->pendingDrawsLocked();
				erase_if(waitDraws, [&](auto* draw) {
					return needsSyncLocked(subm, *draw).empty();
				});

				if(!waitDraws.empty()) {
					auto& waitSems = semaphores.emplace_back(
						si.pWaitSemaphores,
						si.pWaitSemaphores + si.waitSemaphoreCount);
					auto& stages = waitStages.emplace_back(
						si.pWaitDstStageMask,
						si.pWaitDstStageMask + si.waitSemaphoreCount);

					if(tsInfo) {
						auto& waitVals = tsValues.emplace_back(
							tsInfo->pWaitSemaphoreValues,
							tsInfo->pWaitSemaphoreValues + tsInfo->waitSemaphoreValueCount);
						waitVals.resize(si.waitSemaphoreCount); // ignored

						for(auto* draw : waitDraws) {
							waitVals.push_back(draw->futureSemaphoreValue);
							waitSems.push_back(draw->futureSemaphore);
						}

						tsInfo->waitSemaphoreValueCount = u32(waitVals.size());
						tsInfo->pWaitSemaphoreValues = waitVals.data();
					} else {
						// if there are draws with already used semaphores,
						// we have to simply block.
						// TODO: just wait for all draws if we have to wait
						//   for one? for simplicity?
						// TODO: when at least one semaphore was already used
						//   we could simply insert a new one to the gfx queue
						//   (at the current position) and wait for that.
						//   better than waiting for fences...
						std::vector<VkFence> fences;
						erase_if(waitDraws, [&](Draw* draw) {
							if(draw->futureSemaphoreUsed) {
								fences.push_back(draw->fence);
								return true;
							}
							return false;
						});

						if(!fences.empty()) {
							// NOTE waiting inside lock :(
							dlg_trace("Have to wait for gui draws on cpu");
							VK_CHECK(dev.dispatch.WaitForFences(dev.handle,
								u32(fences.size()), fences.data(), true, UINT64_MAX));
							// NOTE: could mark draws as finished in gui.
						}

						for(auto* draw : waitDraws) {
							dlg_assert(!draw->futureSemaphoreUsed);
							draw->futureSemaphoreUsed = true;
							waitSems.push_back(draw->futureSemaphore);
						}
					}

					si.waitSemaphoreCount = u32(waitSems.size());
					si.pWaitSemaphores = waitSems.data();
					si.pWaitDstStageMask = stages.data();
				}
			}
		}

		// = signal semaphores =
		// We need to add a semaphore for device synchronization.
		// We might wanna read from resources that are potentially written
		// by this submission in the future, we need to be able to gpu-sync them.
		dst.ourSemaphore = getSemaphoreFromPool(dev, checkedSubmissions,
			resettedSemaphores, queue);

		auto& signalSems = semaphores.emplace_back(
			si.pSignalSemaphores,
			si.pSignalSemaphores + si.signalSemaphoreCount);
		signalSems.push_back(dst.ourSemaphore);

		if(tsInfo) {
			u64 ourValue;
			VK_CHECK(dev.dispatch.GetSemaphoreCounterValue(dev.handle, dst.ourSemaphore, &ourValue));
			++ourValue;

			dst.ourSemaphoreValue = ourValue;

			auto& signalVals = tsValues.emplace_back(
				tsInfo->pSignalSemaphoreValues,
				tsInfo->pSignalSemaphoreValues + tsInfo->signalSemaphoreValueCount);
			signalVals.resize(si.signalSemaphoreCount); // ignored
			signalVals.push_back(ourValue);

			tsInfo->signalSemaphoreValueCount = u32(signalVals.size());
			tsInfo->pSignalSemaphoreValues = signalVals.data();
		}

		si.signalSemaphoreCount = u32(signalSems.size());
		si.pSignalSemaphores = signalSems.data();

		si.commandBufferCount = u32(commandBuffers.back().size());
		si.pCommandBuffers = commandBuffers.back().data();

		nsubmitInfos.push_back(si);
	}

	// Make sure that every submission has a fence associated.
	// If the application already set a fence we can simply check that
	// to see if the submission completed (the vulkan spec gives us enough
	// guarantees to allow it). Otherwise we have to use a fence from the pool.
	VkFence submFence;
	if(fence) {
		subm.appFence = &dev.fences.get(fence);

		std::lock_guard lock(dev.mutex);

		// per vulkan spec, using a fence in QueueSubmit that is signaled
		// is not allowed. And if it was reset we also remove its associated
		// submission.
		dlg_assert(!subm.appFence->submission);
		submFence = fence;
		subm.appFence->submission = &subm;
	} else {
		subm.ourFence = getFenceFromPool(dev, checkedSubmissions);
		submFence = subm.ourFence;
	}

	// Lock order here important, see mutex usage for rendering in window.cpp.
	std::lock_guard queueLock(dev.queueMutex);

	dlg_assert(nsubmitInfos.size() == submitCount);
	auto res = dev.dispatch.QueueSubmit(queue, u32(nsubmitInfos.size()), nsubmitInfos.data(), submFence);
	if(res != VK_SUCCESS) {
		auto resName = vk::name(res);
		dlg_trace("QueueSubmit returned {}", resName ? resName : "<Unknown>");

		if(subm.ourFence) {
			dev.fencePool.push_back(subm.ourFence);
		} else {
			dlg_assert(subm.appFence);
			subm.appFence->submission = nullptr;
		}

		for(auto& subm : subm.submissions) {
			if(subm.ourSemaphore) {
				dev.semaphorePool.push_back(subm.ourSemaphore);
			}
		}

		return res;
	}

	// NOTE: needed to make sure CommandRecord isn't destroyed inside
	// mutex lock.
	std::vector<std::unique_ptr<CommandBufferGroup>> keepAliveGroups;
	std::vector<IntrusivePtr<CommandRecord>> keepAliveRecs;

	{
		std::lock_guard lock(dev.mutex);

		RecordBatch* batch = nullptr;
		if(dev.swapchain) {
			batch = &dev.swapchain->nextFrameSubmissions.emplace_back();
			batch->queue = &qd;
		}

		for(auto& sub : subm.submissions) {

			for(auto& scb : sub.cbs) {
				auto* cb = scb.cb;
				cb->pending.push_back(&subm);
				auto recPtr = cb->lastRecordPtrLocked();

				if(batch) {
					batch->submissions.push_back(recPtr);
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
					keepAliveRecs.push_back(std::move(recPtr->group->lastRecord));
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

		dev.pending.push_back(std::move(submPtr));

		// Remove old command groups, making sure we don't just leak them.
		auto& qf = dev.queueFamilies[qd.family];
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

				keepAliveGroups.push_back(std::move(*it));
				it = qf.commandGroups.erase(it);
			} else {
				++it;
			}
		}
	}

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

VKAPI_ATTR VkResult VKAPI_CALL DeviceWaitIdle(VkDevice device) {
	auto& dev = getData<Device>(device);
	VkResult res;

	{
		// waiting on a device is considered a queue operation, needs
		// queue synchronization.
		std::lock_guard lock(dev.queueMutex);
		res = dev.dispatch.DeviceWaitIdle(device);
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

} // namespace vil
