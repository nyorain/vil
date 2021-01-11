#include <queue.hpp>
#include <data.hpp>
#include <util.hpp>
#include <cb.hpp>
#include <commands.hpp>
#include <sync.hpp>
#include <image.hpp>
#include <vk/enumString.hpp>

namespace fuen {

std::optional<SubmIterator> checkLocked(PendingSubmission& subm) {
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
		for(auto& [cb, _] : sub.cbs) {
			auto it2 = std::find(cb->pending.begin(), cb->pending.end(), &subm);
			dlg_assert(it2 != cb->pending.end());
			cb->pending.erase(it2);
		}

		for(auto& [sem, stage] : sub.waitSemaphores) {
			dlg_assert(sem->waitFrom == &subm);
			sem->waitFrom = nullptr;
		}

		for(auto* sem : sub.signalSemaphores) {
			dlg_assert(sem->signalFrom == &subm);
			sem->signalFrom = nullptr;
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

	return dev.pending.erase(it);
}

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

// WIP
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
	auto submPtr = std::make_unique<PendingSubmission>();
	auto& subm = *submPtr;
	subm.queue = &qd;

	// TODO(perf): we make a lot of allocations here and this is something
	// that can be called multiple times per frame. Should likely use
	// an allocator

	// We might have to modify the submission:
	// - command buffers can be hooked (to allow us inserting/removing/changing
	//   commands), meaning we use an internal command buffer as replacement
	// - we add our own semaphore to every submission to allow tracking
	//   when they are finished (we can use that in the gui to use the resources
	//   ourselves without having to wait on cpu for the submissions to complete)
	std::vector<VkSubmitInfo> nsubmitInfos;
	std::vector<std::vector<VkSemaphore>> signalSemaphores;
	std::vector<std::vector<VkCommandBuffer>> commandBuffers;
	std::vector<std::unique_ptr<std::byte[]>> copiedChains;

	std::vector<std::vector<u64>> tsValues;
	std::vector<VkTimelineSemaphoreSubmitInfo> tsSubmitInfos;

	for(auto i = 0u; i < submitCount; ++i) {
		auto si = pSubmits[i]; // copy it
		auto& dst = subm.submissions.emplace_back();

		for(auto j = 0u; j < si.signalSemaphoreCount; ++j) {
			auto& sem = dev.semaphores.get(si.pSignalSemaphores[j]);
			dst.signalSemaphores.push_back(&sem);

			std::lock_guard lock(dev.mutex);
			sem.signalFrom = &subm;
		}

		auto& cbs = commandBuffers.emplace_back();
		for(auto j = 0u; j < si.commandBufferCount; ++j) {
			auto& cb = dev.commandBuffers.get(si.pCommandBuffers[j]);
			dst.cbs.push_back({});
			auto& scb = dst.cbs.back();
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
						constexpr auto matchThreshold = 0.6; // TODO: make configurable?
						auto matchVal = match(qgroup->desc, rec.desc);
						best = std::max(best, matchVal);
						if(matchVal > matchThreshold) {
							rec.group = qgroup.get();
						}
					}
				}

				// When no existing group matches, just create a new one
				if(!rec.group) {
					auto& qfam = dev.queueFamilies[rec.queueFamily];
					rec.group = qfam.commandGroups.emplace_back(std::make_unique<CommandBufferGroup>()).get();
					qd.groups.insert(rec.group);
					rec.group->queues = {{&qd, qSubmitID}};

					// dlg_info("Created new command group {} (best match {}}:", rec.group, best);
					// print(rec.desc, 1);
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
				if(rec.group->hook) {
					// scb.hook = std::make_unique<CommandHookSubmission>();
					auto hooked = rec.group->hook->hook(cb, subm, scb.hook);
					dlg_assert(hooked);
					cbs.push_back(hooked);
					dlg_assertm(!cb.hook, "Hook registered for command buffer and group");
				} else if(cb.hook) {
					auto hooked = cb.hook->hook(cb, subm, scb.hook);
					dlg_assert(hooked);
					cbs.push_back(hooked);
				} else {
					cbs.push_back(cb.handle());
				}
			}
		}

		for(auto j = 0u; j < si.waitSemaphoreCount; ++j) {
			auto& semaphore = dev.semaphores.get(si.pWaitSemaphores[j]);
			dlg_assert(!semaphore.waitFrom);
			dst.waitSemaphores.emplace_back(&semaphore, si.pWaitDstStageMask[j]);

			std::lock_guard lock(dev.mutex);
			semaphore.waitFrom = &subm;
		}

		// We need to add a semaphore for device synchronization.
		// We might wanna read from resources that are potentially written
		// by this submission in the future, we need to be able to gpu-sync them.
		dst.ourSemaphore = getSemaphoreFromPool(dev, checkedSubmissions,
			resettedSemaphores, queue);

		signalSemaphores.emplace_back(si.pSignalSemaphores,
			si.pSignalSemaphores + si.signalSemaphoreCount);
		signalSemaphores.back().push_back(dst.ourSemaphore);

		if(dev.timelineSemaphores) {
			u64 ourValue;
			VK_CHECK(dev.dispatch.GetSemaphoreCounterValue(dev.handle, dst.ourSemaphore, &ourValue));
			++ourValue;

			dst.ourSemaphoreValue = ourValue;

			// If the application has already appended a timeline semaphore
			// structure, we must re-use it, inserting our value.
			// Otherwise, simply create a new link chain.
			if(hasChain(si, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)) {
				copyChain(si.pNext, copiedChains.emplace_back());
				auto* tsInfo = const_cast<VkTimelineSemaphoreSubmitInfo*>(findChainInfo<
					VkTimelineSemaphoreSubmitInfo,
					VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO>(si));
				dlg_assert(tsInfo);

				auto& signalVals = tsValues.emplace_back(tsInfo->pSignalSemaphoreValues,
					tsInfo->pSignalSemaphoreValues + tsInfo->signalSemaphoreValueCount);
				signalVals.push_back(ourValue);

				tsInfo->signalSemaphoreValueCount = signalVals.size();
				tsInfo->pSignalSemaphoreValues = signalVals.data();
			} else {
				auto& signalVals = tsValues.emplace_back();
				signalVals.resize(si.signalSemaphoreCount); // ignored
				signalVals.push_back(ourValue);

				auto& tsInfo = tsSubmitInfos.emplace_back();
				tsInfo = {};
				tsInfo.pNext = si.pNext;
				tsInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
				tsInfo.signalSemaphoreValueCount = signalVals.size();
				tsInfo.pSignalSemaphoreValues = signalVals.data();

				si.pNext = &tsInfo;
			}
		}

		si.signalSemaphoreCount = u32(signalSemaphores.back().size());
		si.pSignalSemaphores = signalSemaphores.back().data();

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
		dlg_trace("QueueSubmit returned {}", vk::name(res));

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

	// NOTE: need to make sure CommandRecord isn't destroyed inside
	// mutex lock.
	std::vector<IntrusivePtr<CommandRecord>> keepAlive;

	{
		std::lock_guard lock(dev.mutex);

		for(auto& sub : subm.submissions) {
			for(auto& scb : sub.cbs) {
				auto* cb = scb.cb;
				cb->pending.push_back(&subm);
				auto recPtr = cb->lastRecordPtrLocked();

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
					keepAlive.push_back(std::move(recPtr->group->lastRecord));
				}

				recPtr->group->lastRecord = recPtr;
				// TODO: not sure about this in case of	already existent group
				recPtr->group->desc = recPtr->desc;
			}
		}

		dev.pending.push_back(std::move(submPtr));

		// reset old comamnad groups
		// TODO: arbitrary atm! Can be improved.
		auto& qf = dev.queueFamilies[qd.family];
		for(auto it = qf.commandGroups.begin(); it != qf.commandGroups.end();) {
			auto rem = true;
			for(auto& [squeue, lastSubm] : (*it)->queues) {
				if(squeue->submissionCount - lastSubm < 10) {
					rem = false;
					break;
				}
			}

			if(rem) {
				for(auto& [squeue, _] : (*it)->queues) {
					squeue->groups.erase(it->get());
				}

				keepAlive.push_back(std::move((*it)->lastRecord));
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
		if(!nit) {
			dlg_error("Expected submission to be completed after vkDeviceWaitIdle");
			++it;
		}

		it = *nit;
	}

	return res;
}

} // namespace fuen
