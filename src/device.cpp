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
#include <vk/dispatch_table_helper.h>

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
	dlg_trace("Destroying Device");

	// Vulkan spec requires that all pending submissions have finished.
	for(auto& subm : pending) {
		// We don't have to lock the mutex at checkLocked here since
		// there can't be any concurrent calls on the device as it
		// is being destroyed.
		auto res = checkLocked(*subm);
		dlg_assert(res);
	}

	// user must have erased all resources
	dlg_assert(this->swapchains.empty());
	dlg_assert(this->images.empty());
	dlg_assert(this->buffers.empty());
	dlg_assert(this->imageViews.empty());
	dlg_assert(this->framebuffers.empty());
	dlg_assert(this->renderPasses.empty());
	dlg_assert(this->commandPools.empty());
	dlg_assert(this->commandBuffers.empty());
	dlg_assert(this->fences.empty());
	dlg_assert(this->semaphores.empty());
	dlg_assert(this->events.empty());
	dlg_assert(this->dsPools.empty());
	dlg_assert(this->dsLayouts.empty());
	dlg_assert(this->descriptorSets.empty());
	dlg_assert(this->deviceMemories.empty());
	dlg_assert(this->shaderModules.empty());
	dlg_assert(this->buffers.empty());
	dlg_assert(this->graphicsPipes.empty());
	dlg_assert(this->computePipes.empty());
	dlg_assert(this->pipeLayouts.empty());
	dlg_assert(this->queryPools.empty());
	dlg_assert(this->bufferViews.empty());

	if(window) {
		window.reset();
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

	// erase queue datas
	for(auto& queue : this->queues) {
		eraseData(queue->handle);
	}
}

// api
VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(
		VkPhysicalDevice phdev,
		const VkDeviceCreateInfo* ci,
		const VkAllocationCallbacks* alloc,
		VkDevice* pDevice) {
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
	std::vector<VkQueueFamilyProperties> qfprops(nqf);
	fpGetPhysicalDeviceQueueFamilyProperties(phdev, &nqf, qfprops.data());

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

	auto extsBegin = ci->ppEnabledExtensionNames;
	auto extsEnd = ci->ppEnabledExtensionNames + ci->enabledExtensionCount;

	// Make sure we get a queue that can potentially display to our
	// window. To check that, we first have to create a window though.
	std::unique_ptr<DisplayWindow> window;
	u32 presentQueueInfoID = u32(-1);
	auto createWindow = true;
	std::vector<const char*> newExts; // keep-alive
	if(ini.display && createWindow) {
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
			}

			if(presentQueueInfoID == u32(-1)) {
				dlg_warn("Can't create present window since no queue supports presenting to it");
				window.reset();
			} else {
				// If swapchain extension wasn't enabled, enable it!
				// TODO: we can and should probably check if the extension
				// is supported here, first.
				auto extName = std::string_view(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
				auto it = std::find(extsBegin, extsEnd, extName);
				if(it == extsEnd) {
					newExts = {extsBegin, extsEnd};
					newExts.push_back(extName.data());

					dlg_info("Adding {} device extension", extName);

					nci.enabledExtensionCount = newExts.size();
					nci.ppEnabledExtensionNames = newExts.data();
				}
			}
		}
	}

	nci.pQueueCreateInfos = queueCreateInfos.data();
	nci.queueCreateInfoCount = queueCreateInfos.size();

	VkResult result = fpCreateDevice(phdev, &nci, alloc, pDevice);
	if(result != VK_SUCCESS) {
		return result;
	}

	auto& dev = createData<Device>(*pDevice);
	dev.ini = iniData;
	dev.phdev = phdev;
	dev.handle = *pDevice;
	dev.queueProps = std::move(qfprops);
	dev.extensions = {extsBegin, extsEnd};
	if(ci->pEnabledFeatures) {
		dev.enabledFeatures = *ci->pEnabledFeatures;
	}

	layer_init_device_dispatch_table(*pDevice, &dev.dispatch, fpGetDeviceProcAddr);

	dev.swapchains.mutex = &dev.mutex;
	dev.images.mutex = &dev.mutex;
	dev.imageViews.mutex = &dev.mutex;
	dev.buffers.mutex = &dev.mutex;
	dev.framebuffers.mutex = &dev.mutex;
	dev.renderPasses.mutex = &dev.mutex;
	dev.commandBuffers.mutex = &dev.mutex;
	dev.commandPools.mutex = &dev.mutex;
	dev.fences.mutex = &dev.mutex;
	dev.dsPools.mutex = &dev.mutex;
	dev.dsLayouts.mutex = &dev.mutex;
	dev.descriptorSets.mutex = &dev.mutex;
	dev.buffers.mutex = &dev.mutex;
	dev.deviceMemories.mutex = &dev.mutex;
	dev.shaderModules.mutex = &dev.mutex;
	dev.samplers.mutex = &dev.mutex;
	dev.computePipes.mutex = &dev.mutex;
	dev.graphicsPipes.mutex = &dev.mutex;
	dev.pipeLayouts.mutex = &dev.mutex;
	dev.events.mutex = &dev.mutex;
	dev.semaphores.mutex = &dev.mutex;
	dev.queryPools.mutex = &dev.mutex;
	dev.bufferViews.mutex = &dev.mutex;

	// find vkSetDeviceLoaderData callback
	auto* loaderData = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*ci);
	while(loaderData && loaderData->function != VK_LOADER_DATA_CALLBACK) {
		loaderData = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*loaderData);
	}

	dlg_assert(loaderData);
	dev.setDeviceLoaderData = loaderData->u.pfnSetDeviceLoaderData;

	// Other properties
	dev.ini->dispatch.GetPhysicalDeviceProperties(dev.phdev, &dev.props);

	// Get device queues
	for(auto i = 0u; i < queueCreateInfos.size(); ++i) {
		auto& qi = queueCreateInfos[i];
		auto& familyProps = dev.queueProps[qi.queueFamilyIndex];

		dev.usedQueueFamilyIndices.push_back(qi.queueFamilyIndex);

		for(auto j = 0u; j < qi.queueCount; ++j) {
			auto& q = *dev.queues.emplace_back(std::make_unique<Queue>());
			q.dev = &dev;
			q.flags = familyProps.queueFlags;
			q.priority = qi.pQueuePriorities[j];
			dev.dispatch.GetDeviceQueue(dev.handle, qi.queueFamilyIndex, j, &q.handle);

			// Queue is a dispatchable handle.
			// We therefore have to inform the loader that we created this
			// resource inside the layer and let it set its dispatch table.
			// We will also have to get our queue-data just from the VkQueue
			// later on (e.g. vkQueueSubmit) so associate data with it.
			dev.setDeviceLoaderData(dev.handle, q.handle);
			insertData(q.handle, &q);

			if(i == gfxQueueInfoID && j == 0u) {
				dlg_assert(!dev.gfxQueue);
				dev.gfxQueue = &q;
			}

			if(i == presentQueueInfoID && j == 0u) {
				dlg_assert(window);
				dlg_assert(!window->presentQueue);
				window->presentQueue = &q;
			}
		}
	}

	auto newEnd = std::unique(dev.usedQueueFamilyIndices.begin(), dev.usedQueueFamilyIndices.end());
	dev.usedQueueFamilyIndices.erase(newEnd, dev.usedQueueFamilyIndices.end());

	// query memory stuff
	ini.dispatch.GetPhysicalDeviceMemoryProperties(phdev, &dev.memProps);
	for(auto i = 0u; i < dev.memProps.memoryTypeCount; ++i) {
		auto flags = dev.memProps.memoryTypes[i].propertyFlags;
		if(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
			dev.hostVisibleMemTypeBits |= (1 << i);
		}

		if(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
			dev.deviceLocalMemTypeBits |= (1 << i);
		}
	}

	// == graphics-stuff ==
	dev.renderData = std::make_unique<RenderData>();
	dev.renderData->init(dev);

	// command pool
	VkCommandPoolCreateInfo cpci {};
	cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpci.queueFamilyIndex = dev.gfxQueue->family;
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	VK_CHECK(dev.dispatch.CreateCommandPool(dev.handle, &cpci, nullptr, &dev.commandPool));
	nameHandle(dev, dev.commandPool, "Device:commandPool");

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
	VK_CHECK(dev.dispatch.CreateDescriptorPool(dev.handle, &dpci, nullptr, &dev.dsPool));
	nameHandle(dev, dev.dsPool, "Device:dsPool");

	// == window stuff ==
	if(window) {
		dlg_assert(window->presentQueue); // should have been set in queue querying
		dev.window = std::move(window);
		dev.window->initDevice(dev);
	}

	// Make sure we can recognize the VkDevice even when it comes from
	// the application directly (and is potentially wrapped by other
	// layers). See our public API doc/implementation for why we need this.
	storeDeviceByLoader(dev.handle, &dev);

	return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(
		VkDevice dev,
		const VkAllocationCallbacks* alloc) {
	auto devd = moveData<Device>(dev);
	dlg_assert(devd);

	// destroy our logical device before we call the function.
	auto* destroyDev = devd->dispatch.DestroyDevice;
	devd.reset();

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
	nameHandle(dev, fence, "Device:[pool fence]");
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
		nameHandle(dev, semaphore, "Device:[pool semaphore]");
		return semaphore;
	};

	// hook fence
	auto submPtr = std::make_unique<PendingSubmission>();
	auto& subm = *submPtr;
	subm.queue = &qd;

	std::vector<VkSubmitInfo> nsubmitInfos;
	std::vector<std::vector<VkSemaphore>> signalSemaphores;
	std::vector<std::vector<VkCommandBuffer>> commandBuffers;

	for(auto i = 0u; i < submitCount; ++i) {
		auto si = pSubmits[i]; // copy it
		auto& dst = subm.submissions.emplace_back();

		for(auto j = 0u; j < si.signalSemaphoreCount; ++j) {
			dst.signalSemaphores.push_back(si.pSignalSemaphores[j]);
		}

		auto& cbs = commandBuffers.emplace_back();
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

				// hook command buffer
				if(cb.hook) {
					auto hooked = cb.hook(cb);
					dlg_assert(hooked);
					cbs.push_back(hooked);
				} else {
					cbs.push_back(cb.handle);
				}
			}
		}

		for(auto j = 0u; j < si.waitSemaphoreCount; ++j) {
			dst.waitSemaphores.emplace_back(
				si.pWaitSemaphores[j],
				si.pWaitDstStageMask[j]);
		}

		// We need to add a semaphore for device synchronization.
		// We might wanna read from resources that are potentially written
		// by this submission in the future, we need to be able to gpu-sync them.
		dst.ourSemaphore = getSemaphoreFromPool();

		signalSemaphores.emplace_back(dst.signalSemaphores);
		signalSemaphores.back().push_back(dst.ourSemaphore);

		si.signalSemaphoreCount = signalSemaphores.back().size();
		si.pSignalSemaphores = signalSemaphores.back().data();

		si.commandBufferCount = commandBuffers.back().size();
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

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue vkQueue) {
	auto& queue = getData<Queue>(vkQueue);
	auto res = queue.dev->dispatch.QueueWaitIdle(vkQueue);
	if(res != VK_SUCCESS) {
		return res;
	}

	// check all submissions for completion
	std::lock_guard lock(queue.dev->mutex);
	for(auto it = queue.dev->pending.begin(); it != queue.dev->pending.end();) {
		auto& subm = *it;
		if(subm->queue != &queue) {
			++it;
			continue;
		}

		auto res = checkLocked(*subm);
		if(!res) {
			dlg_error("Expected submission to be completed after vkQueueWaitIdle");
			++it;
		}
		// otherwise, don't increase it, since the current element was removed
	}

	return res;
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.DeviceWaitIdle(device);
	if(res != VK_SUCCESS) {
		return res;
	}

	// check all submissions for completion
	std::lock_guard lock(dev.mutex);
	for(auto it = dev.pending.begin(); it != dev.pending.end();) {
		auto& subm = *it;
		auto res = checkLocked(*subm);
		if(!res) {
			dlg_error("Expected submission to be completed after vkDeviceWaitIdle");
			++it;
		}
		// otherwise, don't increase it, since the current element was removed
	}

	return res;
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
	forEachGuiLocked(dev, [&](auto& gui) {
		gui.destroyed(handle);
	});
}

void nameHandle(Device& dev, VkObjectType objType, u64 handle, const char* name) {
	if(!dev.ini->debugUtilsEnabled) {
		return;
	}

	auto name2 = "fuen:" + std::string(name);

	dlg_assert(dev.dispatch.SetDebugUtilsObjectNameEXT);
	VkDebugUtilsObjectNameInfoEXT nameInfo {};
	nameInfo.objectHandle = handle;
	nameInfo.objectType = objType;
	nameInfo.pObjectName = name2.c_str();
	nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
	VK_CHECK(dev.dispatch.SetDebugUtilsObjectNameEXT(dev.handle, &nameInfo));
}

} // namespace fuen