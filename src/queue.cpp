#include <queue.hpp>
#include <data.hpp>
#include <cb.hpp>
#include <ds.hpp>
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
#include <tracy/Tracy.hpp>

namespace vil {

std::optional<SubmIterator> checkLocked(SubmissionBatch& subm) {
	ZoneScoped;

	auto& dev = *subm.queue->dev;
	vil_assert_owned(dev.mutex);

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

	dev.resetSemaphores.insert(dev.resetSemaphores.end(),
		subm.poolSemaphores.begin(), subm.poolSemaphores.end());

	return dev.pending.erase(it);
}

SubmittedCommandBuffer::SubmittedCommandBuffer() = default;
SubmittedCommandBuffer::~SubmittedCommandBuffer() = default;

CommandBufferGroup::CommandBufferGroup() = default;
CommandBufferGroup::~CommandBufferGroup() = default;

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

void checkPendingSubmissionsLocked(Device& dev) {
	vil_assert_owned(dev.mutex);

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
		submitter.submitID = ++qd.submissionCount;

		// Check all pending submissions for completion, to possibly return
		// resources to fence/semaphore pools
		// check all submissions for completion
		checkPendingSubmissionsLocked(dev);
	}

	auto batchPtr = std::make_unique<SubmissionBatch>();
	auto& batch = *batchPtr;
	batch.submissions.reserve(submitCount); // make sure it's never re-allocated
	batch.queue = &qd;
	batch.submitID = submitter.submitID;

	submitter.dstBatch = &batch;

	for(auto i = 0u; i < submitCount; ++i) {
		process(submitter, pSubmits[i]);
	}

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
		// arond it.
		// Maybe we can handle this with a separate gui/submission sync mutex?
		std::lock_guard devLock(dev.mutex);
		if(res = addGuiSyncLocked(submitter); res != VK_SUCCESS) {
			return res;
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
	auto& dev = getDevice(device);
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
