#include <device.hpp>
#include <queue.hpp>
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
	while(!pending.empty()) {
		// We don't have to lock the mutex at checkLocked here since
		// there can't be any concurrent calls on the device as it
		// is being destroyed.
		auto res = checkLocked(*pending[0]);
		dlg_assert(res.has_value());
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
	dlg_assert(this->dsuTemplates.empty());

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

	for(auto& qf : queueFamilies) {
		if(qf.commandPool) {
			dispatch.DestroyCommandPool(handle, qf.commandPool, nullptr);
		}
	}

	// erase queue datas
	for(auto& queue : this->queues) {
		eraseData(queue->handle);
	}
}

std::unique_ptr<DisplayWindow> tryCreateWindow(Instance& ini,
		std::vector<const char*>& devExts,
		std::vector<VkDeviceQueueCreateInfo>& queueCreateInfos,
		PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr, VkPhysicalDevice phdev,
		u32 numQueueFams, u32& presentQueueInfoID) {
	auto env = std::getenv("FUEN_CREATE_WINDOW");
	if(!env || *env == '0') {
		return nullptr;
	}

	std::unique_ptr<DisplayWindow> window;
	window = std::make_unique<DisplayWindow>();

	// TODO: we could choose a specific backend given the enabled
	// instance extensions (this matters on linux where we could hint
	// swa whether it should use x11 or wayland; but also have to tell
	// it whether to use xcb or xlib for window creation).
	if(!window->createDisplay()) {
		return nullptr;
	}

	// check if required extensions happen to be supported
	unsigned nexts;
	auto* exts = swa_display_vk_extensions(window->dpy, &nexts);

	for(auto i = 0u; i < nexts; ++i) {
		auto ext = std::string_view(exts[i]);
		auto it = find(ini.extensions, ext);
		if(it == ini.extensions.end()) {
			dlg_warn("Can't create window since extension {} was not enabled", ext);
			return {};
		}
	}

	if(!window->createWindow(ini)) {
		return nullptr;
	}

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
	static const float prio1 = 1.f;
	if(presentQueueInfoID == u32(-1)) {
		// The application does not create a graphics queue, so we
		// add one.
		for(auto qf = 0u; qf < numQueueFams; ++qf) {
			VkBool32 supported {};
			auto res = fpGetPhysicalDeviceSurfaceSupportKHR(phdev,
				qf, window->surface, &supported);
			if(res == VK_SUCCESS && supported) {
				presentQueueInfoID = u32(queueCreateInfos.size());

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
		auto it = find(devExts, extName);
		if(it == devExts.end()) {
			devExts.push_back(extName.data());

			dlg_info("Adding {} device extension", extName);
		}
	}

	return window;
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
				gfxQueueInfoID = u32(queueCreateInfos.size());

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
	std::vector<const char*> newExts = {extsBegin, extsEnd};

	// Make sure we get a queue that can potentially display to our
	// window. To check that, we first have to create a window though.
	u32 presentQueueInfoID = u32(-1);
	auto window = tryCreateWindow(ini, newExts, queueCreateInfos,
		fpGetInstanceProcAddr, phdev, nqf, presentQueueInfoID);

	nci.enabledExtensionCount = u32(newExts.size());
	nci.ppEnabledExtensionNames = newExts.data();

	nci.pQueueCreateInfos = queueCreateInfos.data();
	nci.queueCreateInfoCount = u32(queueCreateInfos.size());

	VkResult result = fpCreateDevice(phdev, &nci, alloc, pDevice);
	if(result != VK_SUCCESS) {
		return result;
	}

	auto& dev = createData<Device>(*pDevice);
	dev.ini = iniData;
	dev.phdev = phdev;
	dev.handle = *pDevice;
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
	dev.dsuTemplates.mutex = &dev.mutex;

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
		dev.usedQueueFamilyIndices.push_back(qi.queueFamilyIndex);

		for(auto j = 0u; j < qi.queueCount; ++j) {
			auto& q = *dev.queues.emplace_back(std::make_unique<Queue>());
			q.dev = &dev;
			q.objectType = VK_OBJECT_TYPE_QUEUE;
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

	// Create queue families and their command pools
	VkCommandPoolCreateInfo cpci {};
	cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	dev.queueFamilies.resize(nqf);
	for(auto i = 0u; i < nqf; ++i) {
		cpci.queueFamilyIndex = u32(i);

		auto& qfam = dev.queueFamilies[i];
		qfam.props = qfprops[i];

		if(find(dev.usedQueueFamilyIndices, i) != dev.usedQueueFamilyIndices.end()) {
			VK_CHECK(dev.dispatch.CreateCommandPool(dev.handle, &cpci, nullptr, &qfam.commandPool));
			nameHandle(dev, qfam.commandPool,
				dlg::format("Device:queueFam[{}].commandPool", i).c_str());
		}
	}

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
	if(!dev) {
		return;
	}

	auto devd = moveData<Device>(dev);
	dlg_assert(devd);

	// destroy our logical device before we call the function.
	auto* destroyDev = devd->dispatch.DestroyDevice;
	devd.reset();

	destroyDev(dev, alloc);
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
