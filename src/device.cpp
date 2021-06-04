#include <device.hpp>
#include <queue.hpp>
#include <layer.hpp>
#include <data.hpp>
#include <swapchain.hpp>
#include <window.hpp>
#include <handles.hpp>
#include <overlay.hpp>
#include <rt.hpp>
#include <util/util.hpp>
#include <gui/gui.hpp>
#include <gui/commandHook.hpp>
#include <swa/swa.h>
#include <vk/dispatch_table_helper.h>

namespace vil {

DebugStats& DebugStats::get() {
	static DebugStats ret;
	return ret;
}

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

bool hasAppExt(Device& dev, const char* extName) {
	auto it = find(dev.appExts, extName);
	return (it != dev.appExts.end());
}

// deivce
Device::~Device() {
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
	dlg_assert(this->pipes.empty());
	dlg_assert(this->pipeLayouts.empty());
	dlg_assert(this->queryPools.empty());
	dlg_assert(this->bufferViews.empty());
	dlg_assert(this->dsuTemplates.empty());
	dlg_assert(this->accelStructs.empty());

	// their (transitive) destructors may use device resources
	commandHook.reset();
	window.reset();

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
		renderData.reset();
	}

	if(dsPool) {
		dispatch.DestroyDescriptorPool(handle, dsPool, nullptr);
	}

	// erase queue datas
	for(auto& queue : this->queues) {
		if(!queue->createdByUs) {
			eraseData(queue->handle);
		}
	}

	for(auto& qf : queueFamilies) {
		// qf.commandGroups.clear();
		if(qf.commandPool) {
			dispatch.DestroyCommandPool(handle, qf.commandPool, nullptr);
		}
	}

	queueFamilies.clear();
	queues.clear();
}

bool hasExt(span<const VkExtensionProperties> extProps, const char* name) {
	for(auto& prop : extProps) {
		if(!std::strcmp(prop.extensionName, name)) {
			return true;
		}
	}

	return false;
}

std::unique_ptr<DisplayWindow> tryCreateWindow(Instance& ini,
		std::vector<const char*>& devExts,
		std::vector<VkDeviceQueueCreateInfo>& queueCreateInfos,
		PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr, VkPhysicalDevice phdev,
		u32 numQueueFams, u32& presentQueueInfoID,
		span<const VkExtensionProperties> extProps) {
	if(!checkEnvBinary("VIL_CREATE_WINDOW", false)) {
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
		dlg_warn("Can't create gui window since no queue supports presenting to it");
		window.reset();
	} else {
		// If swapchain extension wasn't enabled, enable it.
		auto extName = std::string_view(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		auto it = find(devExts, extName);
		if(it == devExts.end()) {
			if(hasExt(extProps, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
				devExts.push_back(extName.data());
				dlg_info("Adding {} device extension", extName);
			} else {
				dlg_warn("Can't create gui window since swapchain extension is not supported");
				window.reset();
			}
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
	auto fpCreateDevice = (PFN_vkCreateDevice) fpGetInstanceProcAddr(ini.handle, "vkCreateDevice");

	if(!fpCreateDevice) {
		dlg_error("could not load vkCreateDevice");
		return VK_ERROR_INITIALIZATION_FAILED;
	}

  	// Advance the link info for the next element on the chain
	auto mutLinkInfo = const_cast<VkLayerDeviceCreateInfo*>(linkInfo);
   	mutLinkInfo->u.pLayerInfo = linkInfo->u.pLayerInfo->pNext;

   	// query queues
	u32 nqf;
	ini.dispatch.GetPhysicalDeviceQueueFamilyProperties(phdev, &nqf, nullptr);
	std::vector<VkQueueFamilyProperties> qfprops(nqf);
	ini.dispatch.GetPhysicalDeviceQueueFamilyProperties(phdev, &nqf, qfprops.data());

	// query supported device extensions
	u32 numExts;
	ini.dispatch.EnumerateDeviceExtensionProperties(phdev, nullptr, &numExts, nullptr);
	std::vector<VkExtensionProperties> supportedExts(numExts);
	ini.dispatch.EnumerateDeviceExtensionProperties(phdev, nullptr, &numExts,
		supportedExts.data());

	// == Modify create info ==
	auto nci = *ci;

	// copy the pNext chain so we can modify it.
	std::unique_ptr<std::byte[]> copiedChain;
	auto* pNext = copyChain(nci.pNext, copiedChain);
	nci.pNext = pNext;

	// = Queues =
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
		fpGetInstanceProcAddr, phdev, nqf, presentQueueInfoID,
		supportedExts);

	// = Enabled features =
	// We try to additionally enable a couple of features/exts we need:
	// - timeline semaphores
	// - transform feedback
	// - nonSolidFill mode (for vertex viewer lines)

	// core 1.0 features
	VkPhysicalDeviceFeatures supFeatures10 {};
	ini.dispatch.GetPhysicalDeviceFeatures(phdev, &supFeatures10);

	// when the application adds a PhysicalDeviceFeatures2 to pNext
	// we must use it instead of nci.pEnabledFeatures
	VkPhysicalDeviceFeatures enabledFeatures10 {};
	VkPhysicalDeviceFeatures* pEnabledFeatures10 {};

	auto* features2 = findChainInfo<VkPhysicalDeviceFeatures2,
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2>(nci);
	if(features2) {
		// const_cast is allowed since we coied the pNext chain above
		pEnabledFeatures10 = const_cast<VkPhysicalDeviceFeatures*>(&features2->features);
		dlg_assert(!ci->pEnabledFeatures);
	} else {
		if(ci->pEnabledFeatures) {
			enabledFeatures10 = *ci->pEnabledFeatures;
		}
		pEnabledFeatures10 = &enabledFeatures10;
		nci.pEnabledFeatures = &enabledFeatures10;
	}

	if(supFeatures10.fillModeNonSolid)  {
		pEnabledFeatures10->fillModeNonSolid = true;
	} else {
		dlg_warn("fillModeNonSolid not supported, will get ugly vertex viewer");
	}

	// ext features
	VkPhysicalDeviceProperties phdevProps;
	ini.dispatch.GetPhysicalDeviceProperties(phdev, &phdevProps);

	auto fpPhdevFeatures2 = PFN_vkGetPhysicalDeviceFeatures2(nullptr);
	auto fpPhdevProps2 = PFN_vkGetPhysicalDeviceProperties2(nullptr);
	if(ini.vulkan11 && phdevProps.apiVersion >= VK_API_VERSION_1_1) {
		dlg_assert(ini.dispatch.GetPhysicalDeviceFeatures2);
		dlg_assert(ini.dispatch.GetPhysicalDeviceProperties2);
		fpPhdevFeatures2 = ini.dispatch.GetPhysicalDeviceFeatures2;
		fpPhdevProps2 = ini.dispatch.GetPhysicalDeviceProperties2;
	} else if(contains(ini.extensions, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
		dlg_assert(ini.dispatch.GetPhysicalDeviceFeatures2KHR);
		dlg_assert(ini.dispatch.GetPhysicalDeviceProperties2KHR);
		fpPhdevFeatures2 = ini.dispatch.GetPhysicalDeviceFeatures2KHR;
		fpPhdevProps2 = ini.dispatch.GetPhysicalDeviceProperties2KHR;
	}

	// useful to test fallback code paths
	// TODO: when the validation layer version with the timeline semaphores validation crash (it's fixed
	// in master already) is old enough, we should enable timeline semaphores by default.
	auto enableTimelineSemaphoreUsage = checkEnvBinary("VIL_TIMELINE_SEMAPHORES", false);
	auto enableTransformFeedback = checkEnvBinary("VIL_TRANSFORM_FEEDBACK", true);

	auto hasTimelineSemaphoresApi = enableTimelineSemaphoreUsage && (
		(ini.vulkan12 && phdevProps.apiVersion >= VK_API_VERSION_1_2) ||
		hasExt(supportedExts, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME));
	auto hasTransformFeedbackApi = enableTransformFeedback &&
		hasExt(supportedExts, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);

	auto hasTimelineSemaphores = false;
	auto hasTransformFeedback = false;

	// find generally relevant feature structs in chain
	VkPhysicalDeviceVulkan12Features* inVulkan12 = nullptr;
	VkPhysicalDeviceTimelineSemaphoreFeatures* inTS = nullptr;
	VkPhysicalDeviceTransformFeedbackFeaturesEXT* inXFB = nullptr;
	VkPhysicalDeviceBufferDeviceAddressFeatures* inBufAddr = nullptr;

	auto* link = static_cast<VkBaseOutStructure*>(pNext);
	while(link) {
		if(link->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
			dlg_assert(phdevProps.apiVersion >= VK_API_VERSION_1_2);
			inVulkan12 = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(link);
			break;
		} else if(link->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
			inTS = reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(link);
			break;
		} else if(link->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
			inBufAddr = reinterpret_cast<VkPhysicalDeviceBufferDeviceAddressFeatures*>(link);
			break;
		} else if(link->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT) {
			inXFB = reinterpret_cast<VkPhysicalDeviceTransformFeedbackFeaturesEXT*>(link);
			break;
		}

		link = (static_cast<VkBaseOutStructure*>(link->pNext));
	}

	VkPhysicalDeviceTimelineSemaphoreFeatures tsFeatures {};
	VkPhysicalDeviceTransformFeedbackFeaturesEXT tfFeatures {};
	if(hasTimelineSemaphoresApi || hasTransformFeedbackApi) {
		dlg_assert(fpPhdevFeatures2);
		dlg_assert(fpPhdevProps2);

		// query features support
		VkPhysicalDeviceFeatures2 features2 {};
		features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

		VkPhysicalDeviceTimelineSemaphoreFeatures timelineSemFeatures {};
		if(hasTimelineSemaphoresApi) {
			timelineSemFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
			features2.pNext = &timelineSemFeatures;
		}

		VkPhysicalDeviceTransformFeedbackFeaturesEXT transformFeedbackFeatures {};
		if(hasTransformFeedbackApi) {
			transformFeedbackFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
			transformFeedbackFeatures.pNext = features2.pNext;
			features2.pNext = &transformFeedbackFeatures;
		}

		fpPhdevFeatures2(phdev, &features2);

		if(timelineSemFeatures.timelineSemaphore) {
			hasTimelineSemaphores = true;

			// check if application already has a feature struct holding it
			auto addLink = true;
			if(inVulkan12) {
				addLink = false;
				inVulkan12->timelineSemaphore = true;
			} else if(link->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
				addLink = false;
				inTS->timelineSemaphore = true;
			}

			if(addLink) {
				tsFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
				tsFeatures.timelineSemaphore = true;
				tsFeatures.pNext = const_cast<void*>(nci.pNext);
				nci.pNext = &tsFeatures;
			}

			// we might need to enable the extension
			if(!ini.vulkan12 || phdevProps.apiVersion < VK_API_VERSION_1_2) {
				dlg_assert(hasExt(supportedExts, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME));
				if(!contains(newExts, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) {
					newExts.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
				}
			}
		}

		if(transformFeedbackFeatures.transformFeedback) {
			hasTransformFeedback = true;

			// check if application already has a feature struct holding it
			auto addLink = true;
			if(inXFB) {
				addLink = false;
				inXFB->transformFeedback = true;
			}

			if(addLink) {
				tfFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
				tfFeatures.transformFeedback = true;
				tfFeatures.pNext = const_cast<void*>(nci.pNext);
				nci.pNext = &tfFeatures;
			}

			// also need to enable extension
			if(!contains(newExts, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME)) {
				newExts.push_back(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
			}
		}

		// TODO: could find out props, e.g. for transform feedback
		// VkPhysicalDeviceProperties2 phProps2 {};
		// phProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		// fpPhdevProps2(phdev, &phProps2);
	}

	// = Useful extensions =
	auto checkEnable = [&](const char* name) {
		if(hasExt(supportedExts, name)) {
			if(!contains(newExts, name)) {
				newExts.push_back(name);
			}

			return true;
		}

		return false;
	};

	checkEnable(VK_AMD_SHADER_INFO_EXTENSION_NAME);

	if(fpPhdevProps2) {
		checkEnable(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
	}

	// == Create Device! ==
	nci.enabledExtensionCount = u32(newExts.size());
	nci.ppEnabledExtensionNames = newExts.data();

	nci.pQueueCreateInfos = queueCreateInfos.data();
	nci.queueCreateInfoCount = u32(queueCreateInfos.size());

	VkResult result = fpCreateDevice(phdev, &nci, alloc, pDevice);
	if(result != VK_SUCCESS) {
		return result;
	}

	auto devHandle = *pDevice;
	Device* pDev;
	if(HandleDesc<VkDevice>::wrap) {
		auto* wrapped = new WrappedHandle<Device>();
		pDev = &wrapped->obj();
		*pDevice = castDispatch<VkDevice>(*pDev, *wrapped);
	} else {
		pDev = &createData<Device>(*pDevice);
	}

	auto& dev = *pDev;
	dev.ini = iniData;
	dev.phdev = phdev;
	dev.handle = devHandle;
	dev.props = phdevProps;

	dev.timelineSemaphores = hasTimelineSemaphores;
	dev.transformFeedback = hasTransformFeedback;
	dev.nonSolidFill = pEnabledFeatures10->fillModeNonSolid;

	// NOTE: we don't ever enable buffer device address and just
	// want to track whether it's enabled.
	if(inVulkan12) {
		dev.bufferDeviceAddress = inVulkan12->bufferDeviceAddress;
	} else if(inBufAddr) {
		dev.bufferDeviceAddress = inBufAddr->bufferDeviceAddress;
	}

	dev.appExts = {extsBegin, extsEnd};
	dev.allExts = {newExts.begin(), newExts.end()};

	if(ci->pEnabledFeatures) {
		dev.enabledFeatures = *ci->pEnabledFeatures;
	}

	layer_init_device_dispatch_table(dev.handle, &dev.dispatch, fpGetDeviceProcAddr);

	// TODO: no idea exactly why this is needed. I guess they should not be
	// part of the device loader table in the first place?
	// Might be related: https://github.com/KhronosGroup/Vulkan-Loader/issues/116
	dev.dispatch.QueueBeginDebugUtilsLabelEXT = (PFN_vkQueueBeginDebugUtilsLabelEXT)
		fpGetInstanceProcAddr(ini.handle, "vkQueueBeginDebugUtilsLabelEXT");
	dev.dispatch.QueueEndDebugUtilsLabelEXT = (PFN_vkQueueEndDebugUtilsLabelEXT)
		fpGetInstanceProcAddr(ini.handle, "vkQueueEndDebugUtilsLabelEXT");
	dev.dispatch.CmdInsertDebugUtilsLabelEXT = (PFN_vkCmdInsertDebugUtilsLabelEXT)
		fpGetInstanceProcAddr(ini.handle, "vkCmdInsertDebugUtilsLabelEXT");
	dev.dispatch.CmdBeginDebugUtilsLabelEXT = (PFN_vkCmdBeginDebugUtilsLabelEXT)
		fpGetInstanceProcAddr(ini.handle, "vkCmdBeginDebugUtilsLabelEXT");
	dev.dispatch.CmdEndDebugUtilsLabelEXT = (PFN_vkCmdEndDebugUtilsLabelEXT)
		fpGetInstanceProcAddr(ini.handle, "vkCmdEndDebugUtilsLabelEXT");
	dev.dispatch.SetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT)
		fpGetInstanceProcAddr(ini.handle, "vkSetDebugUtilsObjectNameEXT");
	dev.dispatch.SetDebugUtilsObjectTagEXT = (PFN_vkSetDebugUtilsObjectTagEXT)
		fpGetInstanceProcAddr(ini.handle, "vkSetDebugUtilsObjectTagEXT");
	dev.dispatch.QueueInsertDebugUtilsLabelEXT = (PFN_vkQueueInsertDebugUtilsLabelEXT)
		fpGetInstanceProcAddr(ini.handle, "vkQueueInsertDebugUtilsLabelEXT");

	// NOTE: not sure if this is needed actually.
	// Should do it for all commands that need it for now.
	// We are also doing this in instance.
	aliasCmd(std::array{
		&dev.dispatch.GetSemaphoreCounterValue,
		&dev.dispatch.GetSemaphoreCounterValueKHR});
	aliasCmd(std::array{
		&dev.dispatch.SignalSemaphore,
		&dev.dispatch.SignalSemaphoreKHR});
	aliasCmd(std::array{
		&dev.dispatch.WaitSemaphores,
		&dev.dispatch.WaitSemaphoresKHR});
	aliasCmd(std::array{
		&dev.dispatch.CmdDispatchBase,
		&dev.dispatch.CmdDispatchBaseKHR});
	aliasCmd(std::array{
		&dev.dispatch.CmdDrawIndexedIndirectCount,
		&dev.dispatch.CmdDrawIndexedIndirectCountKHR,
		&dev.dispatch.CmdDrawIndexedIndirectCountAMD});
	aliasCmd(std::array{
		&dev.dispatch.CmdBeginRenderPass2,
		&dev.dispatch.CmdBeginRenderPass2KHR});
	aliasCmd(std::array{
		&dev.dispatch.CmdNextSubpass2,
		&dev.dispatch.CmdNextSubpass2KHR});
	aliasCmd(std::array{
		&dev.dispatch.CmdEndRenderPass2,
		&dev.dispatch.CmdEndRenderPass2KHR});
	aliasCmd(std::array{
		&dev.dispatch.TrimCommandPool,
		&dev.dispatch.TrimCommandPoolKHR});
	// KHR_descriptor_update_template, vulkan 1.1
	aliasCmd(std::array{
		&dev.dispatch.CreateDescriptorUpdateTemplate,
		&dev.dispatch.CreateDescriptorUpdateTemplateKHR});
	aliasCmd(std::array{
		&dev.dispatch.DestroyDescriptorUpdateTemplate,
		&dev.dispatch.DestroyDescriptorUpdateTemplateKHR});
	aliasCmd(std::array{
		&dev.dispatch.UpdateDescriptorSetWithTemplate,
		&dev.dispatch.UpdateDescriptorSetWithTemplateKHR});
	/// {KHR, EXT}_buffer_device_addres, Vulkan 1.2
	aliasCmd(std::array{
		&dev.dispatch.GetBufferDeviceAddress,
		&dev.dispatch.GetBufferDeviceAddressKHR,
		&dev.dispatch.GetBufferDeviceAddressEXT});
	aliasCmd(std::array{
		&dev.dispatch.GetBufferOpaqueCaptureAddress,
		&dev.dispatch.GetBufferOpaqueCaptureAddressKHR});
	aliasCmd(std::array{
		&dev.dispatch.GetDeviceMemoryOpaqueCaptureAddress,
		&dev.dispatch.GetDeviceMemoryOpaqueCaptureAddressKHR});

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
	dev.pipes.mutex = &dev.mutex;
	dev.pipeLayouts.mutex = &dev.mutex;
	dev.events.mutex = &dev.mutex;
	dev.semaphores.mutex = &dev.mutex;
	dev.queryPools.mutex = &dev.mutex;
	dev.bufferViews.mutex = &dev.mutex;
	dev.dsuTemplates.mutex = &dev.mutex;
	dev.accelStructs.mutex = &dev.mutex;

	// find vkSetDeviceLoaderData callback
	auto* loaderData = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*ci);
	while(loaderData && loaderData->function != VK_LOADER_DATA_CALLBACK) {
		loaderData = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*loaderData);
	}

	dlg_assert(loaderData);
	dev.setDeviceLoaderData = loaderData->u.pfnSetDeviceLoaderData;

	// Get device queues
	for(auto i = 0u; i < queueCreateInfos.size(); ++i) {
		auto& qi = queueCreateInfos[i];
		dev.usedQueueFamilyIndices.push_back(qi.queueFamilyIndex);

		for(auto j = 0u; j < qi.queueCount; ++j) {
			auto& q = *dev.queues.emplace_back(std::make_unique<Queue>());
			q.dev = &dev;
			q.objectType = VK_OBJECT_TYPE_QUEUE;
			q.priority = qi.pQueuePriorities[j];
			q.family = qi.queueFamilyIndex;
			q.createdByUs = (i >= ci->queueCreateInfoCount);
			dev.dispatch.GetDeviceQueue(dev.handle, qi.queueFamilyIndex, j, &q.handle);

			// Queue is a dispatchable handle.
			// We therefore have to inform the loader that we created this
			// resource inside the layer and let it set its dispatch table.
			// We will also have to get our queue-data just from the VkQueue
			// later on (e.g. vkQueueSubmit) so associate data with it.
			dev.setDeviceLoaderData(dev.handle, q.handle);

			// We can never get calls from the application using queues
			// that we created.
			if(!q.createdByUs) {
				insertData(q.handle, &q);
			}

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
	// TODO: proper resource management.
	VkDescriptorPoolSize poolSizes[2];
	poolSizes[0].descriptorCount = 50u;
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

	poolSizes[1].descriptorCount = 10u;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

	VkDescriptorPoolCreateInfo dpci {};
	dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpci.poolSizeCount = 2u;
	dpci.pPoolSizes = poolSizes;
	dpci.maxSets = 50u;
	dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	VK_CHECK(dev.dispatch.CreateDescriptorPool(dev.handle, &dpci, nullptr, &dev.dsPool));
	nameHandle(dev, dev.dsPool, "Device:dsPool");

	// init command hook
	dev.commandHook = std::make_unique<CommandHook>(dev);

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
		VkDevice vkDevice,
		const VkAllocationCallbacks* alloc) {
	if(!vkDevice) {
		return;
	}

	if(HandleDesc<VkDevice>::wrap) {
		auto& wrapped = unwrapWrapped(vkDevice);
		auto& dev = wrapped.obj();

		// destroy our logical device before we forward the call
		auto handle = dev.handle;
		auto* destroyDev = dev.dispatch.DestroyDevice;
		delete &wrapped;

		destroyDev(handle, alloc);
	} else {
		auto devd = moveData<Device>(vkDevice);
		dlg_assert(devd);

		// destroy our logical device before we forward the call
		auto handle = devd->handle;
		auto* destroyDev = devd->dispatch.DestroyDevice;
		devd.reset();

		destroyDev(handle, alloc);
	}
}

// util
void notifyDestruction(Device& dev, Handle& handle) {
	std::lock_guard lock(dev.mutex);
	if(dev.gui) {
		dev.gui->destroyed(handle);
	}
}

void nameHandle(Device& dev, VkObjectType objType, u64 handle, const char* name) {
	if(!dev.ini->debugUtilsEnabled || !dev.dispatch.SetDebugUtilsObjectNameEXT) {
		return;
	}

	auto name2 = "vil:" + std::string(name);

	VkDebugUtilsObjectNameInfoEXT nameInfo {};
	nameInfo.objectHandle = handle;
	nameInfo.objectType = objType;
	nameInfo.pObjectName = name2.c_str();
	nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
	VK_CHECK(dev.dispatch.SetDebugUtilsObjectNameEXT(dev.handle, &nameInfo));
}

DebugLabel::DebugLabel(Device& dev, VkCommandBuffer cb, const char* name,
		std::array<float, 4> color) {
	if(!dev.ini->debugUtilsEnabled || !dev.dispatch.CmdBeginDebugUtilsLabelEXT) {
		return;
	}

	dev_ = &dev;
	cb_ = cb;

	VkDebugUtilsLabelEXT labelInfo {};
	labelInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
	labelInfo.pLabelName = name;
	std::copy(color.begin(), color.end(), labelInfo.color);
	dev.dispatch.CmdBeginDebugUtilsLabelEXT(cb, &labelInfo);
}

DebugLabel::~DebugLabel() {
	if(dev_) {
		dlg_assert(cb_);
		dlg_assert(dev_->dispatch.CmdEndDebugUtilsLabelEXT);
		dev_->dispatch.CmdEndDebugUtilsLabelEXT(cb_);
	}
}

bool Device::BufferAddressCmp::operator()(const Buffer* a, const Buffer* b) const {
	dlg_assert(a && a->deviceAddress);
	dlg_assert(b && b->deviceAddress);
	if(a->deviceAddress == b->deviceAddress) {
		// this serves as a tie breaker.
		// This is mainly relevant for memory aliasing.
		return a < b;
	}

	return a->deviceAddress < b->deviceAddress;
}

bool Device::BufferAddressCmp::operator()(VkDeviceAddress a, const Buffer* b) const {
	dlg_assert(b && b->deviceAddress);
	return a < b->deviceAddress;
}

bool Device::BufferAddressCmp::operator()(const Buffer* a, VkDeviceAddress b) const {
	dlg_assert(a && a->deviceAddress && a->ci.size > 0u);
	return a->deviceAddress + a->ci.size <= b;
}

// Defined here (instead of util/util.hpp) since they access Device
bool supportedUsage(VkFormatFeatureFlags features, VkImageUsageFlags usages, bool has11) {
	static constexpr struct {
		VkImageUsageFlagBits usage;
		VkFormatFeatureFlagBits feature;
	} maps[] = {
		{VK_IMAGE_USAGE_SAMPLED_BIT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
		{VK_IMAGE_USAGE_STORAGE_BIT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT},
		{VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT},
		{VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT},
	};

	for(const auto& map : maps) {
		if((usages & map.usage) && !(features & map.feature)) {
			return false;
		}
	}

	// The transfer features were only added in vulkan 1.1
	if(has11) {
		if((usages & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && !(features & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
			return false;
		}

		if((usages & VK_IMAGE_USAGE_TRANSFER_DST_BIT) && !(features & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
			return false;
		}
	}

	return true;
}

bool supported(const Device& dev, const VkImageCreateInfo& info, VkFormatFeatureFlags additional) {
	VkFormatProperties props;
	dev.ini->dispatch.GetPhysicalDeviceFormatProperties(dev.phdev, info.format, &props);
	auto features = (info.tiling == VK_IMAGE_TILING_LINEAR) ?
		props.linearTilingFeatures : props.optimalTilingFeatures;

	VkImageFormatProperties formatProps;
	auto res = dev.ini->dispatch.GetPhysicalDeviceImageFormatProperties(
		dev.phdev, info.format, info.imageType, info.tiling,
		info.usage, info.flags, &formatProps);
	if(res == VK_ERROR_FORMAT_NOT_SUPPORTED) {
		return false;
	} else if(res != VK_SUCCESS) {
		dlg_error("Unexpected error from getPhysicalDeviceImageFormatProperties: {}",
			unsigned(res));
		return false;
	}

	auto vulkan11 = dev.ini->vulkan11 && (dev.props.apiVersion > VK_API_VERSION_1_1);
	return ((features & additional) == additional &&
		supportedUsage(features, info.usage, vulkan11) &&
		info.extent.width <= formatProps.maxExtent.width &&
		info.extent.height <= formatProps.maxExtent.height &&
		info.extent.depth <= formatProps.maxExtent.depth &&
		info.mipLevels <= formatProps.maxMipLevels &&
		info.arrayLayers <= formatProps.maxArrayLayers &&
		(formatProps.sampleCounts & info.samples));
}

VkFormat findSupported(const Device& dev, span<const VkFormat> formats,
		const VkImageCreateInfo& info, VkFormatFeatureFlags additional) {
	auto copy = info;
	for(auto format : formats) {
		copy.format = format;
		if(supported(dev, copy, additional)) {
			return format;
		}
	}

	return VK_FORMAT_UNDEFINED;
}

VkFormat findDepthFormat(const Device& dev) {
	VkImageCreateInfo img {}; // dummy for property checking
	img.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	img.extent = {64, 64, 1};
	img.mipLevels = 1;
	img.arrayLayers = 1;
	img.imageType = VK_IMAGE_TYPE_2D;
	img.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	img.tiling = VK_IMAGE_TILING_OPTIMAL;
	img.samples = VK_SAMPLE_COUNT_1_BIT;
	img.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	auto fmts = {
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_D32_SFLOAT_S8_UINT,
		VK_FORMAT_D24_UNORM_S8_UINT,
		VK_FORMAT_D16_UNORM,
		VK_FORMAT_D16_UNORM_S8_UINT,
	};
	auto features =
		VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
		VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	return findSupported(dev, fmts, img, features);
}


} // namespace vil
