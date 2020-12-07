#include <device.hpp>
#include <layer.hpp>
#include <data.hpp>
#include <util.hpp>
#include <swapchain.hpp>
#include <window.hpp>
#include <commands.hpp>
#include <handles.hpp>
#include <overlay.hpp>
#include <gui/gui.hpp>

#include <swa/swa.h>
#include <vulkan/vk_dispatch_table_helper.h>

namespace fuen {

// util
Gui* getWindowGui(Device& dev) {
	if(dev.window) {
		return &dev.window->gui;
	}

	return nullptr;
}

Gui* getOverlayGui(Swapchain& swapchain) {
	if(swapchain.overlay) {
		return &swapchain.overlay->gui;
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
		dispatch.DestroyFence(handle, fence, nullptr);
	}

	for(auto& semaphore : semaphorePool) {
		dispatch.DestroySemaphore(handle, semaphore, nullptr);
	}

	for(auto& semaphore : resetSemaphores) {
		dispatch.DestroySemaphore(handle, semaphore, nullptr);
	}

	if(renderData) {
		renderData->free(*this);
	}

	if(dsPool) {
		dispatch.DestroyDescriptorPool(handle, dsPool, nullptr);
	}

	if(commandPool) {
		dispatch.DestroyCommandPool(handle, commandPool, nullptr);
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
	auto& ini = *iniData;

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
	auto fpCreateDevice = (PFN_vkCreateDevice)fpGetInstanceProcAddr(ini.handle, "vkCreateDevice");
	if(!fpCreateDevice) {
		dlg_error("could not load vkCreateDevice");
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	auto fpGetPhysicalDeviceQueueFamilyProperties =
		(PFN_vkGetPhysicalDeviceQueueFamilyProperties)
		fpGetInstanceProcAddr(ini.handle, "vkGetPhysicalDeviceQueueFamilyProperties");
	dlg_assert(fpGetPhysicalDeviceQueueFamilyProperties);

  	// Advance the link info for the next element on the chain
	auto mutLinkInfo = const_cast<VkLayerDeviceCreateInfo*>(linkInfo);
   	mutLinkInfo->u.pLayerInfo = linkInfo->u.pLayerInfo->pNext;

   	// query queues
	u32 nqf;
	fpGetPhysicalDeviceQueueFamilyProperties(phdev, &nqf, nullptr);
	auto qfprops = std::make_unique<VkQueueFamilyProperties[]>(nqf);
	fpGetPhysicalDeviceQueueFamilyProperties(phdev, &nqf, qfprops.get());

	// == Modify create info ==
	auto nci = *ci;

	// == Extensions ==
	// TODO: enable extensions useful for us; if supported

	// == Queues ==
	// Make sure we get a graphics queue.
	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos = {
		nci.pQueueCreateInfos,
		nci.pQueueCreateInfos + nci.queueCreateInfoCount
	};

	u32 gfxQueueInfoID = u32(-1);

	// First try to identify an application-created queue that has
	// the graphics flag.
	for(auto i = 0u; i < queueCreateInfos.size(); ++i) {
		auto& q = queueCreateInfos[i];
		if(q.queueCount == 0) {
			continue;
		}

		auto flags = qfprops[q.queueFamilyIndex].queueFlags;
		if(gfxQueueInfoID == u32(-1) && flags & VK_QUEUE_GRAPHICS_BIT) {
			gfxQueueInfoID = i;
			break;
		}
	}

	const float prio1 = 1.f;
	if(gfxQueueInfoID == u32(-1)) {
		// The application does not create a graphics queue, so we
		// add one.
		for(auto qf = 0u; qf < nqf; ++qf) {
			if(qfprops[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
				gfxQueueInfoID = queueCreateInfos.size();

				dlg_trace("Adding new queue (for graphics), family {}", qf);
				auto& q = queueCreateInfos.emplace_back();
				q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
				q.pQueuePriorities = &prio1;
				q.queueFamilyIndex = qf;
				q.queueCount = 1u;
				break;
			}
		}

		dlg_assertm(gfxQueueInfoID != u32(-1),
			"The vulkan implementation exposes no graphics queue!");
	}

	// Make sure we get a queue that can potentially display to our
	// window. To check that, we first have to create a window though.
	std::unique_ptr<DisplayWindow> window;
	u32 presentQueueInfoID = u32(-1);
	if(ini.display) {
		// create window
		window = std::make_unique<DisplayWindow>();
		if(!window->createWindow(ini)) {
			window.reset();
		} else {
			// Find present queue
			auto vkSurf64 = swa_window_get_vk_surface(window->window);
			window->surface = bit_cast<VkSurfaceKHR>(vkSurf64);

			auto fpGetPhysicalDeviceSurfaceSupportKHR =
				(PFN_vkGetPhysicalDeviceSurfaceSupportKHR)
				fpGetInstanceProcAddr(ini.handle, "vkGetPhysicalDeviceSurfaceSupportKHR");
			dlg_assert(fpGetPhysicalDeviceSurfaceSupportKHR);

			// Check queues that are already created for presentation support.
			for(auto i = 0u; i < queueCreateInfos.size(); ++i) {
				auto& q = queueCreateInfos[i];
				if(presentQueueInfoID == u32(-1)) {
					VkBool32 supported {};
					auto res = fpGetPhysicalDeviceSurfaceSupportKHR(phdev,
						q.queueFamilyIndex, window->surface, &supported);
					if(res == VK_SUCCESS && supported) {
						presentQueueInfoID = i;
						break;
					}
				}
			}

			// If none found, add our own
			if(presentQueueInfoID == u32(-1)) {
				// The application does not create a graphics queue, so we
				// add one.
				for(auto qf = 0u; qf < nqf; ++qf) {
					VkBool32 supported {};
					auto res = fpGetPhysicalDeviceSurfaceSupportKHR(phdev,
						qf, window->surface, &supported);
					if(res == VK_SUCCESS && supported) {
						presentQueueInfoID = queueCreateInfos.size();

						dlg_trace("Adding new queue (for present), family {}", qf);
						auto& q = queueCreateInfos.emplace_back();
						q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
						q.pQueuePriorities = &prio1;
						q.queueFamilyIndex = qf;
						q.queueCount = 1u;
						break;
					}
				}

				if(presentQueueInfoID == u32(-1)) {
					dlg_warn("Can't create present window since no queue supports presenting to it");
					window.reset();
				}
			}
		}
	}

	nci.pQueueCreateInfos = queueCreateInfos.data();
	nci.queueCreateInfoCount = queueCreateInfos.size();

	VkResult result = fpCreateDevice(phdev, &nci, alloc, dev);
	if(result != VK_SUCCESS) {
		return result;
	}

	auto& devData = createData<Device>(*dev);
	devData.ini = iniData;
	devData.phdev = phdev;
	devData.handle = *dev;

	layer_init_device_dispatch_table(*dev, &devData.dispatch, fpGetDeviceProcAddr);

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

	// Get device queues
	for(auto i = 0u; i < queueCreateInfos.size(); ++i) {
		auto& qi = queueCreateInfos[i];
		auto& familyProps = qfprops[qi.queueFamilyIndex];

		for(auto j = 0u; j < qi.queueCount; ++j) {
			auto& q = *devData.queues.emplace_back(std::make_unique<Queue>());
			q.dev = &devData;
			q.flags = familyProps.queueFlags;
			q.priority = qi.pQueuePriorities[j];
			devData.dispatch.GetDeviceQueue(*dev, qi.queueFamilyIndex, j, &q.queue);

			// Queue is a dispatchable handle.
			// We therefore have to inform the loader that we created this
			// resource inside the layer and let it set its dispatch table.
			// We will also have to get our queue-data just from the VkQueue
			// later on (e.g. vkQueueSubmit) so associate data with it.
			devData.setDeviceLoaderData(*dev, q.queue);
			insertData(q.queue, &q);

			if(i == gfxQueueInfoID && j == 0u) {
				dlg_assert(!devData.gfxQueue);
				devData.gfxQueue = &q;
			}

			if(i == presentQueueInfoID && j == 0u) {
				dlg_assert(devData.window);
				dlg_assert(!devData.window->presentQueue);
				devData.window->presentQueue = &q;
			}
		}
	}

	// query memory stuff
	VkPhysicalDeviceMemoryProperties memProps;
	ini.dispatch.GetPhysicalDeviceMemoryProperties(phdev, &memProps);
	for(auto i = 0u; i < memProps.memoryTypeCount; ++i) {
		auto flags = memProps.memoryTypes[i].propertyFlags;
		if(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
			devData.hostVisibleMemTypeBits |= (1 << i);
		}

		if(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
			devData.deviceLocalMemTypeBits |= (1 << i);
		}
	}

	// == graphics-stuff ==
	devData.renderData = std::make_unique<RenderData>();
	devData.renderData->init(devData);

	// command pool
	VkCommandPoolCreateInfo cpci {};
	cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpci.queueFamilyIndex = devData.gfxQueue->family;
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	VK_CHECK(devData.dispatch.CreateCommandPool(*dev, &cpci, nullptr, &devData.commandPool));

	// descriptor pool
	// TODO: might need multiple pools...
	VkDescriptorPoolSize poolSize;
	poolSize.descriptorCount = 50u;
	poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

	VkDescriptorPoolCreateInfo dpci {};
	dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpci.pPoolSizes = &poolSize;
	dpci.poolSizeCount = 1u;
	dpci.maxSets = 50u;
	dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	VK_CHECK(devData.dispatch.CreateDescriptorPool(*dev, &dpci, nullptr, &devData.dsPool));

	// == window stuff ==
	if(window) {
		dlg_assert(window->presentQueue); // should have been set in queue querying
		devData.window = std::move(window);
		devData.window->initDevice(devData);
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
	auto* destroyDev = devd->dispatch.DestroyDevice;
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
	VkFenceCreateInfo fci {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VK_CHECK(dev.dispatch.CreateFence(dev.handle, &fci, nullptr, &fence));
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

				VkSubmitInfo si {};
				si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
				si.waitSemaphoreCount = resetSemaphores.size();
				si.pWaitSemaphores = resetSemaphores.data();
				si.pWaitDstStageMask = waitFlags.data();

				lock.unlock();
				auto fence = getFenceFromPool(dev, checkedSubmissions);

				{
					std::lock_guard queueLock(dev.queueMutex);
					VK_CHECK(dev.dispatch.QueueSubmit(queue, 1, &si, fence));
					VK_CHECK(dev.dispatch.WaitForFences(dev.handle, 1, &fence, true, UINT64_MAX));
				}

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
		}

		// create new semaphore
		VkSemaphore semaphore;
		VkSemaphoreCreateInfo sci {};
		sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		VK_CHECK(dev.dispatch.CreateSemaphore(dev.handle, &sci, nullptr, &semaphore));
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
	return dev.dispatch.QueueSubmit(queue, nsubmitInfos.size(), nsubmitInfos.data(), submFence);
}

bool checkLocked(PendingSubmission& subm) {
	auto& dev = *subm.queue->dev;

	if(subm.appFence) {
		if(dev.dispatch.GetFenceStatus(dev.handle, subm.appFence->handle) != VK_SUCCESS) {
			return false;
		}
	} else {
		dlg_assert(subm.ourFence);
		if(dev.dispatch.GetFenceStatus(dev.handle, subm.ourFence) != VK_SUCCESS) {
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
		dev.dispatch.ResetFences(dev.handle, 1, &subm.ourFence);
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
