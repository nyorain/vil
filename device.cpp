#include "device.hpp"
#include "layer.hpp"
#include "data.hpp"
#include "util.hpp"
#include "gui.hpp"
#include "renderer.hpp"
#include "swapchain.hpp"
#include "window.hpp"
#include "cb.hpp"
#include "rp.hpp"
#include "commands.hpp"
#include "memory.hpp"
#include "image.hpp"
#include "buffer.hpp"
#include "pipe.hpp"
#include "shader.hpp"
#include "ds.hpp"
#include "sync.hpp"
#include "overlay.hpp"

namespace fuen {

// util
Renderer* getWindowRenderer(Device& dev) {
	if(dev.window) {
		return &dev.window->renderer;
	}

	return nullptr;
}

Renderer* getOverlayRenderer(Swapchain& swapchain) {
	if(swapchain.overlay) {
		return &swapchain.overlay->renderer;
	}

	return nullptr;
}

// deivce
Device::~Device() {
	for(auto& subm : pending) {
		// we don't have to lock the mutex at checkLocked here since
		// there can't be any concurrent calls on the device as it
		// is being destroyed.
		auto res = checkLocked(*subm);
		dlg_assert(res);
	}

	for(auto& fence : fencePool) {
		dispatch.vkDestroyFence(handle, fence, nullptr);
	}

	for(auto& semaphore : semaphorePool) {
		dispatch.vkDestroySemaphore(handle, semaphore, nullptr);
	}

	for(auto& semaphore : resetSemaphores) {
		dispatch.vkDestroySemaphore(handle, semaphore, nullptr);
	}

	if(renderData) {
		renderData->free(*this);
	}

	if(dsPool) {
		dispatch.vkDestroyDescriptorPool(handle, dsPool, nullptr);
	}

	if(commandPool) {
		dispatch.vkDestroyCommandPool(handle, commandPool, nullptr);
	}
}

// api
VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(
		VkPhysicalDevice phdev,
		const VkDeviceCreateInfo* ci,
		const VkAllocationCallbacks* alloc,
		VkDevice* dev) {
	auto* iniData = findData<Instance>(phdev);
	dlg_assert(iniData);

	auto* linkInfo = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*ci);
	while(linkInfo && linkInfo->function != VK_LAYER_LINK_INFO) {
		linkInfo = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*linkInfo);
	}

	if(!linkInfo) {
		dlg_error("No linkInfo");
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	auto fpGetInstanceProcAddr = linkInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	auto fpGetDeviceProcAddr = linkInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	auto fpCreateDevice = (PFN_vkCreateDevice)fpGetInstanceProcAddr(iniData->handle, "vkCreateDevice");
	if(!fpCreateDevice) {
		dlg_error("could not load vkCreateDevice");
		return VK_ERROR_INITIALIZATION_FAILED;
	}

  	// Advance the link info for the next element on the chain
	auto mutLinkInfo = const_cast<VkLayerDeviceCreateInfo*>(linkInfo);
   	mutLinkInfo->u.pLayerInfo = linkInfo->u.pLayerInfo->pNext;

	VkResult result = fpCreateDevice(phdev, ci, alloc, dev);
	if(result != VK_SUCCESS) {
		return result;
	}

	auto& devData = createData<Device>(*dev);
	devData.ini = iniData;
	devData.phdev = phdev;
	devData.handle = *dev;
	devData.dispatch = iniData->dispatch;
	devData.dispatch.vkGetInstanceProcAddr = fpGetInstanceProcAddr;
	devData.dispatch.vkGetDeviceProcAddr = fpGetDeviceProcAddr;
	devData.dispatch.init((vk::Device) *dev);
	// In case GetDeviceProcAddr of the next chain didn't return itself
	devData.dispatch.vkGetDeviceProcAddr = fpGetDeviceProcAddr;

  	dlg_info("fuen set last device");

	devData.swapchains.mutex = &devData.mutex;
	devData.images.mutex = &devData.mutex;
	devData.imageViews.mutex = &devData.mutex;
	devData.buffers.mutex = &devData.mutex;
	devData.framebuffers.mutex = &devData.mutex;
	devData.renderPasses.mutex = &devData.mutex;
	devData.commandBuffers.mutex = &devData.mutex;
	devData.commandPools.mutex = &devData.mutex;
	devData.fences.mutex = &devData.mutex;
	devData.dsPools.mutex = &devData.mutex;
	devData.dsLayouts.mutex = &devData.mutex;
	devData.descriptorSets.mutex = &devData.mutex;
	devData.buffers.mutex = &devData.mutex;
	devData.deviceMemories.mutex = &devData.mutex;
	devData.shaderModules.mutex = &devData.mutex;
	devData.samplers.mutex = &devData.mutex;
	devData.computePipes.mutex = &devData.mutex;
	devData.graphicsPipes.mutex = &devData.mutex;
	devData.pipeLayouts.mutex = &devData.mutex;
	devData.events.mutex = &devData.mutex;
	devData.semaphores.mutex = &devData.mutex;

	// find vkSetDeviceLoaderData callback
	auto* loaderData = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*ci);
	while(loaderData && loaderData->function != VK_LOADER_DATA_CALLBACK) {
		loaderData = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*loaderData);
	}

	dlg_assert(loaderData);
	devData.setDeviceLoaderData = loaderData->u.pfnSetDeviceLoaderData;

   	// query queues
	u32 nqf;
	devData.dispatch.vkGetPhysicalDeviceQueueFamilyProperties(phdev, &nqf, nullptr);
	auto qfprops = std::make_unique<VkQueueFamilyProperties[]>(nqf);
	devData.dispatch.vkGetPhysicalDeviceQueueFamilyProperties(phdev, &nqf, qfprops.get());

	// reserve vector so we don't get a reallocation
	auto nQueues = 0u;
	for(auto i = 0u; i < ci->queueCreateInfoCount; ++i) {
		nQueues += ci->pQueueCreateInfos[i].queueCount;
	}

	devData.queues.reserve(nQueues);
	for(auto i = 0u; i < ci->queueCreateInfoCount; ++i) {
		auto& qi = ci->pQueueCreateInfos[i];
		auto& familyProps = qfprops[qi.queueFamilyIndex];

		for(auto j = 0u; j < qi.queueCount; ++j) {
			auto& q = *devData.queues.emplace_back(std::make_unique<Queue>());
			q.dev = &devData;
			q.flags = familyProps.queueFlags;
			devData.dispatch.vkGetDeviceQueue(*dev, qi.queueFamilyIndex, j, &q.queue);

			// Queue is a dispatchable handle.
			// We therefore have to inform the loader that we created this
			// resource inside the layer and let it set its dispatch table.
			// We will also have to get our queue-data just from the VkQueue
			// later on (e.g. vkQueueSubmit) so associate data with it.
			devData.setDeviceLoaderData(*dev, q.queue);
			insertData(q.queue, &q);

			if(!devData.gfxQueue && familyProps.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
				devData.gfxQueue = &q;
			}
		}
	}

	// query memory stuff
	VkPhysicalDeviceMemoryProperties memProps;
	devData.dispatch.vkGetPhysicalDeviceMemoryProperties(phdev, &memProps);
	for(auto i = 0u; i < memProps.memoryTypeCount; ++i) {
		auto flags = memProps.memoryTypes[i].propertyFlags;
		if(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
			devData.hostVisibleMemTypeBits |= (1 << i);
		}

		if(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
			devData.deviceLocalMemTypeBits |= (1 << i);
		}
	}

	if(devData.gfxQueue) {
		devData.renderData = std::make_unique<RenderData>();
		devData.renderData->init(devData);

		// command pool
		VkCommandPoolCreateInfo cpci = vk::CommandPoolCreateInfo();
		cpci.queueFamilyIndex = devData.gfxQueue->family;
		cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		VK_CHECK(devData.dispatch.vkCreateCommandPool(*dev, &cpci, nullptr, &devData.commandPool));

		// descriptor pool
		// TODO: might need multiple pools...
		VkDescriptorPoolSize poolSize;
		poolSize.descriptorCount = 50u;
		poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

		VkDescriptorPoolCreateInfo dpci = vk::DescriptorPoolCreateInfo();
		dpci.pPoolSizes = &poolSize;
		dpci.poolSizeCount = 1u;
		dpci.maxSets = 50u;
		dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		VK_CHECK(devData.dispatch.vkCreateDescriptorPool(*dev, &dpci, nullptr, &devData.dsPool));
	}

	constexpr auto useWindow = false;
	if(useWindow) {
		devData.window = std::make_unique<DisplayWindow>();
		devData.window->init(devData);
	}

	return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(
		VkDevice dev,
		VkAllocationCallbacks* alloc) {
	auto devd = moveData<Device>(dev);
	dlg_assert(devd);

	// user must have erased all resources
	dlg_assert(devd->swapchains.empty());
	dlg_assert(devd->images.empty());
	dlg_assert(devd->buffers.empty());
	dlg_assert(devd->imageViews.empty());
	dlg_assert(devd->framebuffers.empty());
	dlg_assert(devd->renderPasses.empty());
	dlg_assert(devd->commandPools.empty());
	dlg_assert(devd->commandBuffers.empty());
	dlg_assert(devd->fences.empty());
	dlg_assert(devd->dsPools.empty());
	dlg_assert(devd->dsLayouts.empty());
	dlg_assert(devd->descriptorSets.empty());
	dlg_assert(devd->deviceMemories.empty());
	dlg_assert(devd->shaderModules.empty());
	dlg_assert(devd->buffers.empty());

	// erase queue datas
	for(auto& queue : devd->queues) {
		eraseData(queue->queue);
	}

	// destroy our logical device before we call the function.
	auto* destroyDev = devd->dispatch.vkDestroyDevice;
	devd.release();
	destroyDev(dev, alloc);
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
	VkFenceCreateInfo fci = vk::FenceCreateInfo();
	VK_CHECK(dev.dispatch.vkCreateFence(dev.handle, &fci, nullptr, &fence));
	return fence;
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

	// We don't declare this as function since it assumes it's called
	// inside QueueSubmit: We might make our own queue submit call to
	// reset semaphores. Doing this outside a QueueSubmit would need additional
	// synchronization.
	auto getSemaphoreFromPool = [&]{
		{
			std::unique_lock lock(dev.mutex);
			if(!dev.semaphorePool.empty()) {
				auto ret = dev.semaphorePool.back();
				dev.semaphorePool.pop_back();
				return ret;
			}

			// If there are enough semaphores in the reset pool, it's worth
			// resetting it.
			// TODO: this is somewhat error-prone. But without it, we
			// might create a shitload of semaphore when the application submits
			// a lot of command buffers without us ever rendering the overlay.
			constexpr auto minResetSemCount = 10u;
			if(!resettedSemaphores && dev.resetSemaphores.size() > minResetSemCount) {
				if(!checkedSubmissions) {
					// first check whether any pending submissions have finished.
					for(auto it = dev.pending.begin(); it < dev.pending.end();) {
						auto& subm = *it;
						if(!subm->ourFence || !checkLocked(*subm)) {
							++it;
						}
					}

					checkedSubmissions = true;
				}

				auto resetSemaphores = std::move(dev.resetSemaphores);
				auto waitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
				std::vector<VkPipelineStageFlags> waitFlags(resetSemaphores.size(), waitStage);

				VkSubmitInfo si = vk::SubmitInfo();
				si.waitSemaphoreCount = resetSemaphores.size();
				si.pWaitSemaphores = resetSemaphores.data();
				si.pWaitDstStageMask = waitFlags.data();

				lock.unlock();
				auto fence = getFenceFromPool(dev, checkedSubmissions);

				{
					std::lock_guard queueLock(dev.queueMutex);
					VK_CHECK(dev.dispatch.vkQueueSubmit(queue, 1, &si, fence));
					VK_CHECK(dev.dispatch.vkWaitForFences(dev.handle, 1, &fence, true, UINT64_MAX));
				}

				auto ret = resetSemaphores.back();
				resetSemaphores.pop_back();
				dev.dispatch.vkResetFences(dev.handle, 1, &fence);

				lock.lock();
				dev.fencePool.push_back(fence);

				dev.semaphorePool.insert(dev.semaphorePool.end(),
					resetSemaphores.begin(), resetSemaphores.end());

				resettedSemaphores = true;
				return ret;
			}
		}

		// create new semaphore
		VkSemaphore semaphore;
		VkSemaphoreCreateInfo sci = vk::SemaphoreCreateInfo();
		VK_CHECK(dev.dispatch.vkCreateSemaphore(dev.handle, &sci, nullptr, &semaphore));
		return semaphore;
	};

	// hook fence
	auto submPtr = std::make_unique<PendingSubmission>();
	auto& subm = *submPtr;
	subm.queue = &qd;

	std::vector<VkSubmitInfo> nsubmitInfos;
	std::vector<std::vector<VkSemaphore>> signalSemaphores;

	for(auto i = 0u; i < submitCount; ++i) {
		auto si = pSubmits[i]; // copy it
		auto& dst = subm.submissions.emplace_back();

		for(auto j = 0u; j < si.signalSemaphoreCount; ++j) {
			dst.signalSemaphore.push_back(si.pSignalSemaphores[j]);
		}

		for(auto j = 0u; j < si.commandBufferCount; ++j) {
			auto& cb = dev.commandBuffers.get(si.pCommandBuffers[j]);
			dst.cbs.push_back(&cb);

			{
				std::lock_guard lock(dev.mutex);

				// store in command buffer that it was submitted here
				cb.pending.push_back(&subm);

				// store pending layouts
				for(auto& used : cb.images) {
					if(used.second.layoutChanged) {
						dlg_assert(
							used.second.finalLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
							used.second.finalLayout != VK_IMAGE_LAYOUT_PREINITIALIZED);

						used.second.image->pendingLayout = used.second.finalLayout;
					}
				}
			}

			// We need to add a semaphore for device synchronization.
			// We might wanna read from resources that are potentially written
			// by this submission in the future, we need to be able to gpu-sync them.
			dst.ourSemaphore = getSemaphoreFromPool();

			signalSemaphores.emplace_back(dst.signalSemaphore);
			signalSemaphores.back().push_back(dst.ourSemaphore);

			si.signalSemaphoreCount = signalSemaphores.back().size();
			si.pSignalSemaphores = signalSemaphores.back().data();
			nsubmitInfos.push_back(si);
		}

		for(auto j = 0u; j < si.waitSemaphoreCount; ++j) {
			dst.waitSemaphores.emplace_back(
				si.pWaitSemaphores[j],
				si.pWaitDstStageMask[j]);
		}
	}

	// Make sure that every submission has a fence associated.
	// If the application already set a fence we can simply check that
	// to see if the submission completed (the vulkan spec gives us enough
	// guarantees to allow it). Otherwise we have to use a fence from the pool.
	VkFence submFence;
	if(fence) {
		subm.appFence = &dev.fences.get(fence);

		std::lock_guard lock(dev.mutex);

		// per vulkan spec, using a fence in QueueSubmit that is signals
		// is not allowed. And if it was reset we also reset its associated
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

	{
		std::lock_guard lock(dev.mutex);
		dev.pending.push_back(std::move(submPtr));
	}

	dlg_assert(nsubmitInfos.size() == submitCount);
	return dev.dispatch.vkQueueSubmit(queue, nsubmitInfos.size(), nsubmitInfos.data(), submFence);
}

bool checkLocked(PendingSubmission& subm) {
	auto& dev = *subm.queue->dev;

	if(subm.appFence) {
		if(dev.dispatch.vkGetFenceStatus(dev.handle, subm.appFence->handle) != VK_SUCCESS) {
			return false;
		}
	} else {
		dlg_assert(subm.ourFence);
		if(dev.dispatch.vkGetFenceStatus(dev.handle, subm.ourFence) != VK_SUCCESS) {
			return false;
		}
	}
	// apparently unique_ptr == ptr comparision not supported in stdlibc++ yet?
	auto it = std::find_if(dev.pending.begin(), dev.pending.end(), [&](auto& ptr){
			return ptr.get() == &subm;
	});
	dlg_assert(it != dev.pending.end());

	for(auto& sub : subm.submissions) {
		for(auto* cb : sub.cbs) {
			auto it2 = std::find(cb->pending.begin(), cb->pending.end(), &subm);
			dlg_assert(it2 != cb->pending.end());
			cb->pending.erase(it2);
		}

		// We don't immediately reset the semaphore since it's not that cheap.
		// We will do it with the next rendering or when there are a lot
		// of semaphores pending.
		dev.resetSemaphores.push_back(sub.ourSemaphore);
	}

	if(subm.ourFence) {
		dev.dispatch.vkResetFences(dev.handle, 1, &subm.ourFence);
		dev.fencePool.push_back(subm.ourFence);
	} else if(subm.appFence) {
		subm.appFence->submission = nullptr;
	}

	dev.pending.erase(it);
	return true;
}

void notifyDestruction(Device& dev, Handle& handle) {
	std::lock_guard lock(dev.mutex);
	forEachGuiLocked(dev, [&](auto& renderer) {
		renderer.unselect(handle);
	});
}

} // namespace fuen
