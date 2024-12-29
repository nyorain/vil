#include <device.hpp>
#include <wrap.hpp>
#include <queue.hpp>
#include <layer.hpp>
#include <data.hpp>
#include <swapchain.hpp>
#include <image.hpp>
#include <buffer.hpp>
#include <queryPool.hpp>
#include <cb.hpp>
#include <rp.hpp>
#include <ds.hpp>
#include <sync.hpp>
#include <swapchain.hpp>
#include <overlay.hpp>
#include <accelStruct.hpp>
#include <threadContext.hpp>
#include <fault.hpp>
#include <util/util.hpp>
#include <gui/gui.hpp>
#include <commandHook/hook.hpp>
#include <vk/dispatch_table_helper.h>

#ifdef VIL_WITH_SWA
	#include <swa/swa.h>
	#include <window.hpp>
#else
	namespace vil { struct DisplayWindow {}; }
#endif // VIL_WITH_SWA

namespace vil {

DebugStats& DebugStats::get() {
	static DebugStats ret;
	return ret;
}

// util
bool hasAppExt(Device& dev, const char* extName) {
	auto it = find(dev.appExts, extName);
	return (it != dev.appExts.end());
}

VkResult VKAPI_PTR SetDeviceLoaderDataNOOP(VkDevice, void*) {
	// noop
	return VK_SUCCESS;
}

// device
Device::Device() {
	auto& dev = *this;
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
}

Device::~Device() {
	// Vulkan spec requires that all pending submissions have finished.
	while(!pending.empty()) {
		// We don't have to lock the mutex at checkLocked here since
		// there can't be any concurrent calls on the device as it
		// is being destroyed.
		auto res = checkLocked(*pending[0]);
		dlg_assert(res.has_value());
	}

	// destroy all resources only kept alive by us
	this->keepAliveBuffers.clear();
	this->keepAliveImageViews.clear();
	this->keepAliveBufferViews.clear();
	this->keepAliveSamplers.clear();
	this->keepAliveAccelStructs.clear();

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
	dlg_assert(!swapchain_.get());

	// Their (transitive) destructors may use device resources.
	// Important to first destroy the window, otherwise rendering may
	// still be active and use e.g. commandHook resources.
	window.reset();
	gui_.reset();
	commandHook.reset();

	for(auto& fence : fencePool) {
		dispatch.DestroyFence(handle, fence, nullptr);
	}

	for(auto& semaphore : semaphorePool) {
		dispatch.DestroySemaphore(handle, semaphore, nullptr);
	}

	for(auto& semaphore : resetSemaphores) {
		dispatch.DestroySemaphore(handle, semaphore, nullptr);
	}

	if(handle) {
		dispatch.DestroyDescriptorPool(handle, dsPool, nullptr);
		dispatch.DestroySampler(handle, nearestSampler, nullptr);
		dispatch.DestroySampler(handle, linearSampler, nullptr);
	}

	// erase queue datas
	for(auto& queue : this->queues) {
		if(!queue->createdByUs) {
			eraseData(queue->handle);
		}

		dispatch.DestroySemaphore(handle, queue->submissionSemaphore, nullptr);
	}

	for(auto& qf : queueFamilies) {
		if(qf.commandPool) {
			dispatch.DestroyCommandPool(handle, qf.commandPool, nullptr);
		}
	}

	queueFamilies.clear();
	queues.clear();
}

Gui& Device::getOrCreateGui(VkFormat colorFormat) {
	std::lock_guard lock(this->mutex);
	if(!gui_) {
		// TODO: don't create Gui object while holding lock?
		gui_ = std::make_unique<Gui>(*this, colorFormat);
	} else {
		gui_->updateColorFormat(colorFormat);
	}

	return *gui_;
}

IntrusivePtr<Swapchain> Device::swapchain() {
	std::lock_guard lock(this->mutex);
	return swapchain_;
}

IntrusivePtr<Swapchain> Device::swapchainPtrLocked() {
	assertOwned(this->mutex);
	return swapchain_;
}

void Device::swapchain(IntrusivePtr<Swapchain> newSwapchain) {
	std::lock_guard lock(this->mutex);
	swapchain_ = std::move(newSwapchain);
}

void Device::swapchainDestroyed(const Swapchain& swapchain) {
	std::lock_guard lock(this->mutex);
	if(swapchain_ == &swapchain) {
		swapchain_.reset();
	}
}

// util
bool hasExt(span<const VkExtensionProperties> extProps, const char* name) {
	for(auto& prop : extProps) {
		if(!std::strcmp(prop.extensionName, name)) {
			return true;
		}
	}

	return false;
}

#ifdef VIL_WITH_SWA
std::unique_ptr<DisplayWindow> tryCreateWindow(Instance& ini,
		std::vector<const char*>& devExts,
		std::vector<VkDeviceQueueCreateInfo>& queueCreateInfos,
		PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr, VkPhysicalDevice phdev,
		u32 numQueueFams, u32& presentQueueInfoID,
		span<const VkExtensionProperties> extProps,
		bool standaloneMode) {
	(void) presentQueueInfoID;
	(void) queueCreateInfos;
	(void) phdev;
	(void) fpGetInstanceProcAddr;
	(void) numQueueFams;

	auto hookOverlay = checkEnvBinary("VIL_HOOK_OVERLAY", false);
	if(!checkEnvBinary("VIL_CREATE_WINDOW", !hookOverlay) && !standaloneMode) {
		return nullptr;
	}

	std::unique_ptr<DisplayWindow> window;
	window = std::make_unique<DisplayWindow>();

	// TODO: we could choose a specific backend given the enabled
	// instance extensions (this matters on linux where we could hint
	// swa whether it should use x11 or wayland; but also have to tell
	// it whether to use xcb or xlib for window creation).
	if(standaloneMode) {
		if(!window->doCreateDisplay()) {
			return nullptr;
		}
	} else {
		if(!window->createDisplay()) {
			return nullptr;
		}
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

	return window;
}
#endif // VIL_WITH_SWA

// api
VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(
		VkPhysicalDevice phdev,
		const VkDeviceCreateInfo* ci,
		const VkAllocationCallbacks* alloc,
		VkDevice* pDevice) {
	return doCreateDevice(phdev, ci, alloc, pDevice);
}

VkResult doCreateDevice(
		VkPhysicalDevice phdev,
		const VkDeviceCreateInfo* ci,
		const VkAllocationCallbacks* alloc,
		VkDevice* pDevice, StandaloneDeviceInfo* standalone) {
	auto* iniData = findData<Instance>(phdev);
	dlg_assert(iniData);
	auto& ini = *iniData;

	PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr;
	if(standalone) {
		fpGetInstanceProcAddr = standalone->getInstanceProcAddr;
		fpGetDeviceProcAddr = standalone->getDeviceProcAddr;
	} else {
		auto* linkInfo = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*ci);
		while(linkInfo && linkInfo->function != VK_LAYER_LINK_INFO) {
			linkInfo = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*linkInfo);
		}

		if(!linkInfo) {
			dlg_error("No linkInfo");
			return VK_ERROR_INITIALIZATION_FAILED;
		}

		auto mutLinkInfo = const_cast<VkLayerDeviceCreateInfo*>(linkInfo);
		fpGetInstanceProcAddr = linkInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
		fpGetDeviceProcAddr = linkInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;

		// Advance the link info for the next element on the chain
		mutLinkInfo->u.pLayerInfo = linkInfo->u.pLayerInfo->pNext;
	}

	dlg_assert(fpGetInstanceProcAddr);
	dlg_assert(fpGetDeviceProcAddr);
	auto fpCreateDevice = (PFN_vkCreateDevice) fpGetInstanceProcAddr(ini.handle, "vkCreateDevice");

	if(!fpCreateDevice) {
		dlg_error("could not load vkCreateDevice");
		return VK_ERROR_INITIALIZATION_FAILED;
	}

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

	const auto unsupportedExts = std::set<std::string>{
		// extensions that are not supported and will most likely cause crashes
		"VK_NV_ray_tracing",
		"VK_NV_shading_rate_image",
		"VK_EXT_transform_feedback",
		"VK_AMD_buffer_marker",
		"VK_NV_device_diagnostic_checkpoints",
		"VK_NV_scissor_exclusive",
		"VK_NV_mesh_shader",
		"VK_INTEL_performance_query",
		"VK_NV_fragment_shading_rate_enums",
		"VK_NV_device_generated_commands",
	};

	constexpr auto checkForUnsupportedExts = true;
	if(checkForUnsupportedExts && !checkEnvBinary("VIL_SKIP_EXT_CHECK", false)) {
		for(auto& ext : newExts) {
			if(unsupportedExts.find(ext) != unsupportedExts.end()) {
				dlg_error("Application requested '{}' but extension is not supported by vil.\n"
					"You can run again with env variable VIL_SKIP_EXT_CHECK=1 to try anyways", ext);
				return VK_ERROR_EXTENSION_NOT_PRESENT;
			}
		}
	}

	std::unique_ptr<DisplayWindow> window;
#ifdef VIL_WITH_SWA
	// When running in standalone mode we want the mainthread to
	// handle the window main loop, externally.
	auto standaloneMode = (standalone != nullptr);
	// Make sure we get a queue that can potentially display to our
	// window. To check that, we first have to create a window though.
	auto presentQueueInfoID = u32(-1);
	window = tryCreateWindow(ini, newExts, queueCreateInfos,
		fpGetInstanceProcAddr, phdev, nqf, presentQueueInfoID,
		supportedExts, standaloneMode);
#endif // VIL_WITH_SWA

	// = Enabled features =
	// We try to additionally enable a couple of features/exts we need:
	// - timeline semaphores
	// - transform feedback
	// - nonSolidFill mode (for vertex viewer lines)
	// - shaderStorageImageWriteWithoutFormat

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
		// const_cast is allowed since we copied the pNext chain above
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

	if(supFeatures10.shaderStorageImageWriteWithoutFormat) {
		pEnabledFeatures10->shaderStorageImageWriteWithoutFormat = true;
	} else {
		dlg_warn("shaderStorageImageWriteWithoutFormat not supported, image copying will be bad");
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
	auto enableTimelineSemaphoreUsage = checkEnvBinary("VIL_TIMELINE_SEMAPHORES", true);
	auto enableTransformFeedback = checkEnvBinary("VIL_TRANSFORM_FEEDBACK", true);
	auto enableDeviceFault = checkEnvBinary("VIL_DEVICE_FAULT", true);

	auto hasTimelineSemaphoresApi = enableTimelineSemaphoreUsage &&
		fpPhdevFeatures2 && fpPhdevProps2 && (
		(ini.vulkan12 && phdevProps.apiVersion >= VK_API_VERSION_1_2) ||
		hasExt(supportedExts, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME));
	auto hasTransformFeedbackApi = enableTransformFeedback &&
		fpPhdevFeatures2 && fpPhdevProps2 &&
		hasExt(supportedExts, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
	auto hasDeviceFaultApi = enableDeviceFault &&
		fpPhdevFeatures2 && fpPhdevProps2 &&
		hasExt(supportedExts, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
	auto hasAddressBindingReportAPI = enableDeviceFault &&
		contains(ini.extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) &&
		fpPhdevFeatures2 && fpPhdevProps2 &&
		hasExt(supportedExts, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME);

	auto hasTimelineSemaphores = false;
	auto hasTransformFeedback = false;
	auto hasDeviceFault = false;
	auto hasAddressBindingReport = false;

	// find generally relevant feature structs in chain
	VkPhysicalDeviceVulkan11Features features11 {};
	VkPhysicalDeviceVulkan12Features features12 {};
	VkPhysicalDeviceVulkan13Features features13 {};

	VkPhysicalDeviceVulkan12Features* inVulkan12 = nullptr;
	VkPhysicalDeviceTimelineSemaphoreFeatures* inTS = nullptr;
	VkPhysicalDeviceTransformFeedbackFeaturesEXT* inXFB = nullptr;
	VkPhysicalDeviceBufferDeviceAddressFeatures* inBufAddr = nullptr;
	VkPhysicalDeviceFaultFeaturesEXT* inDF = nullptr;
	VkPhysicalDeviceAddressBindingReportFeaturesEXT* inABR = nullptr;

	auto* link = static_cast<VkBaseOutStructure*>(pNext);
	while(link) {
		if(link->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
			dlg_assert(phdevProps.apiVersion >= VK_API_VERSION_1_1);
			features11 = *reinterpret_cast<VkPhysicalDeviceVulkan11Features*>(link);
		} else if(link->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
			dlg_assert(phdevProps.apiVersion >= VK_API_VERSION_1_2);
			inVulkan12 = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(link);
			features12 = *reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(link);
		} else if(link->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
			dlg_assert(phdevProps.apiVersion >= VK_API_VERSION_1_3);
			features13 = *reinterpret_cast<VkPhysicalDeviceVulkan13Features*>(link);
		} else if(link->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
			inTS = reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(link);
		} else if(link->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES) {
			inBufAddr = reinterpret_cast<VkPhysicalDeviceBufferDeviceAddressFeatures*>(link);
		} else if(link->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT) {
			inXFB = reinterpret_cast<VkPhysicalDeviceTransformFeedbackFeaturesEXT*>(link);
		} else if(link->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT) {
			inDF = reinterpret_cast<VkPhysicalDeviceFaultFeaturesEXT*>(link);
		} else if(link->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT) {
			inABR = reinterpret_cast<VkPhysicalDeviceAddressBindingReportFeaturesEXT*>(link);
		}

		link = (static_cast<VkBaseOutStructure*>(link->pNext));
	}

	VkPhysicalDeviceTimelineSemaphoreFeatures tsFeatures {};
	VkPhysicalDeviceTransformFeedbackFeaturesEXT tfFeatures {};
	VkPhysicalDeviceFaultFeaturesEXT dfFeatures {};
	VkPhysicalDeviceAddressBindingReportFeaturesEXT abrFeatures {};
	if(hasTimelineSemaphoresApi || hasTransformFeedbackApi || hasDeviceFaultApi) {
		dlg_assert(fpPhdevFeatures2);
		dlg_assert(fpPhdevProps2);

		// query features support
		VkPhysicalDeviceFeatures2 features2 {};
		features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

		if(hasTimelineSemaphoresApi) {
			tsFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
			features2.pNext = &tsFeatures;
		}

		if(hasTransformFeedbackApi) {
			tfFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
			tfFeatures.pNext = features2.pNext;
			features2.pNext = &tfFeatures;
		}

		if(hasDeviceFaultApi) {
			dfFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
			dfFeatures.pNext = features2.pNext;
			features2.pNext = &dfFeatures;

			if(hasAddressBindingReportAPI) {
				abrFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT;
				abrFeatures.pNext = features2.pNext;
				features2.pNext = &abrFeatures;
			}
		}

		fpPhdevFeatures2(phdev, &features2);

		if(tsFeatures.timelineSemaphore) {
			hasTimelineSemaphores = true;

			// check if application already has a feature struct holding it
			auto addLink = true;
			if(inVulkan12) {
				addLink = false;
				inVulkan12->timelineSemaphore = true;
			} else if(inTS) {
				addLink = false;
				inTS->timelineSemaphore = true;
			}

			if(addLink) {
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

		if(tfFeatures.transformFeedback) {
			hasTransformFeedback = true;

			// check if application already has a feature struct holding it
			auto addLink = true;
			if(inXFB) {
				addLink = false;
				inXFB->transformFeedback = true;
			}

			if(addLink) {
				tfFeatures.geometryStreams = false;
				tfFeatures.pNext = const_cast<void*>(nci.pNext);
				nci.pNext = &tfFeatures;
			}

			// also need to enable extension
			if(!contains(newExts, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME)) {
				newExts.push_back(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
			}
		}

		if(dfFeatures.deviceFault) {
			hasDeviceFault = true;

			// check if application already has a feature struct holding it
			if(inDF) {
				inDF->deviceFault = true;
			} else {
				dfFeatures.pNext = const_cast<void*>(nci.pNext);
				nci.pNext = &dfFeatures;
			}

			// also need to enable extension
			if(!contains(newExts, VK_EXT_DEVICE_FAULT_EXTENSION_NAME)) {
				newExts.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
			}

			// device memory address binding report
			// we only enable this extension when we also have the device fault extension. Useless otherwise.
			if (abrFeatures.reportAddressBinding) {
				hasAddressBindingReport = true;

				// check if application already has a feature struct holding it
				if(inABR) {
					inABR->reportAddressBinding = true;
				} else {
					abrFeatures.pNext = const_cast<void*>(nci.pNext);
					nci.pNext = &abrFeatures;
				}

				// also need to enable extension
				if(!contains(newExts, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME)) {
					newExts.push_back(VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME);
				}
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
	dev.shaderStorageImageWriteWithoutFormat = pEnabledFeatures10->shaderStorageImageWriteWithoutFormat;
	dev.extDeviceFault = hasDeviceFault;

	if(hasAddressBindingReport) {
		dev.addressMap = std::make_unique<DeviceAddressMap>();
		dev.addressMap->init(dev);
	}

	// NOTE: we don't ever enable buffer device address and just
	// want to track whether it's enabled.
	if(inVulkan12) {
		dev.bufferDeviceAddress = inVulkan12->bufferDeviceAddress;
	} else if(inBufAddr) {
		dev.bufferDeviceAddress = inBufAddr->bufferDeviceAddress;
	}

	dev.appExts = {extsBegin, extsEnd};
	dev.allExts = {newExts.begin(), newExts.end()};

	dev.enabledFeatures = *pEnabledFeatures10;
	dev.enabledFeatures11 = features11;
	dev.enabledFeatures12 = features12;
	dev.enabledFeatures13 = features13;

	layer_init_device_dispatch_table(dev.handle, &dev.dispatch, fpGetDeviceProcAddr);

	// TODO: no idea exactly why this is needed. I guess they should not be
	// part of the device loader table in the first place?
	// Might be related: https://github.com/KhronosGroup/Vulkan-Loader/issues/116
#define LOAD_FALLBACK_INI(name) if(!dev.dispatch.name) dev.dispatch.name = (PFN_vk##name) \
			fpGetInstanceProcAddr(ini.handle, "vk" #name)

	LOAD_FALLBACK_INI(QueueBeginDebugUtilsLabelEXT);
	LOAD_FALLBACK_INI(QueueEndDebugUtilsLabelEXT);
	LOAD_FALLBACK_INI(CmdInsertDebugUtilsLabelEXT);
	LOAD_FALLBACK_INI(CmdBeginDebugUtilsLabelEXT);
	LOAD_FALLBACK_INI(CmdEndDebugUtilsLabelEXT);
	LOAD_FALLBACK_INI(SetDebugUtilsObjectNameEXT);
	LOAD_FALLBACK_INI(SetDebugUtilsObjectTagEXT);
	LOAD_FALLBACK_INI(QueueInsertDebugUtilsLabelEXT);

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
		&dev.dispatch.CmdDrawIndirectCount,
		&dev.dispatch.CmdDrawIndirectCountAMD,
		&dev.dispatch.CmdDrawIndirectCountKHR});
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
	// create_renderpass_2, vulkan 1.2
	aliasCmd(std::array{
		&dev.dispatch.CreateRenderPass2,
		&dev.dispatch.CreateRenderPass2KHR});
	// KHR_bind_memory_2, vulkan 1.1
	aliasCmd(std::array{
		&dev.dispatch.BindBufferMemory2,
		&dev.dispatch.BindBufferMemory2KHR});
	aliasCmd(std::array{
		&dev.dispatch.BindImageMemory2,
		&dev.dispatch.BindImageMemory2KHR});
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
	// {KHR, EXT}_buffer_device_addres, Vulkan 1.2
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
	// VK_KHR_get_memory_requirements2, Vulkan 1.1
	aliasCmd(std::array{
		&dev.dispatch.GetBufferMemoryRequirements2,
		&dev.dispatch.GetBufferMemoryRequirements2KHR});
	aliasCmd(std::array{
		&dev.dispatch.GetImageMemoryRequirements2,
		&dev.dispatch.GetImageMemoryRequirements2KHR});
	aliasCmd(std::array{
		&dev.dispatch.GetImageSparseMemoryRequirements2,
		&dev.dispatch.GetImageSparseMemoryRequirements2KHR});
	// VK_KHR_synchronization2, vulkan 1.3
	aliasCmd(std::array{
		&dev.dispatch.CmdSetEvent2,
		&dev.dispatch.CmdSetEvent2KHR});
	aliasCmd(std::array{
		&dev.dispatch.CmdResetEvent2,
		&dev.dispatch.CmdResetEvent2KHR});
	aliasCmd(std::array{
		&dev.dispatch.CmdWaitEvents2,
		&dev.dispatch.CmdWaitEvents2KHR});
	aliasCmd(std::array{
		&dev.dispatch.CmdPipelineBarrier2,
		&dev.dispatch.CmdPipelineBarrier2KHR});
	aliasCmd(std::array{
		&dev.dispatch.CmdWriteTimestamp2,
		&dev.dispatch.CmdWriteTimestamp2KHR});
	aliasCmd(std::array{
		&dev.dispatch.QueueSubmit2,
		&dev.dispatch.QueueSubmit2KHR});
	// VK_EXT_extended_dynamic_state
	aliasCmd(std::array{
		&dev.dispatch.CmdSetCullMode,
		&dev.dispatch.CmdSetCullModeEXT});
	aliasCmd(std::array{
		&dev.dispatch.CmdSetFrontFace,
		&dev.dispatch.CmdSetFrontFaceEXT});
	aliasCmd(std::array{
		&dev.dispatch.CmdSetPrimitiveTopology,
		&dev.dispatch.CmdSetPrimitiveTopologyEXT});
	aliasCmd(std::array{
		&dev.dispatch.CmdSetViewportWithCount,
		&dev.dispatch.CmdSetViewportWithCountEXT});
	aliasCmd(std::array{
		&dev.dispatch.CmdSetScissorWithCount,
		&dev.dispatch.CmdSetScissorWithCountEXT});
	aliasCmd(std::array{
		&dev.dispatch.CmdBindVertexBuffers2,
		&dev.dispatch.CmdBindVertexBuffers2EXT});
	aliasCmd(std::array{
		&dev.dispatch.CmdSetDepthTestEnable,
		&dev.dispatch.CmdSetDepthTestEnableEXT});
	aliasCmd(std::array{
		&dev.dispatch.CmdSetDepthWriteEnable,
		&dev.dispatch.CmdSetDepthWriteEnableEXT});
	aliasCmd(std::array{
		&dev.dispatch.CmdSetDepthCompareOp,
		&dev.dispatch.CmdSetDepthCompareOpEXT});
	aliasCmd(std::array{
		&dev.dispatch.CmdSetDepthBoundsTestEnable,
		&dev.dispatch.CmdSetDepthBoundsTestEnableEXT});
	aliasCmd(std::array{
		&dev.dispatch.CmdSetStencilTestEnable,
		&dev.dispatch.CmdSetStencilTestEnableEXT});
	aliasCmd(std::array{
		&dev.dispatch.CmdSetStencilOp,
		&dev.dispatch.CmdSetStencilOpEXT});
	// VK_KHR_copy_commands2
	aliasCmd(std::array{
		&dev.dispatch.CmdCopyBuffer2,
		&dev.dispatch.CmdCopyBuffer2KHR});
	aliasCmd(std::array{
		&dev.dispatch.CmdCopyImage2,
		&dev.dispatch.CmdCopyImage2KHR});
	aliasCmd(std::array{
		&dev.dispatch.CmdCopyBufferToImage2,
		&dev.dispatch.CmdCopyBufferToImage2KHR});
	aliasCmd(std::array{
		&dev.dispatch.CmdCopyImageToBuffer2,
		&dev.dispatch.CmdCopyImageToBuffer2KHR});
	aliasCmd(std::array{
		&dev.dispatch.CmdBlitImage2,
		&dev.dispatch.CmdBlitImage2KHR});
	aliasCmd(std::array{
		&dev.dispatch.CmdResolveImage2,
		&dev.dispatch.CmdResolveImage2KHR});
	// VK_KHR_dynamic_rendering, vulkan 1.3
	aliasCmd(std::array{
		&dev.dispatch.CmdBeginRendering,
		&dev.dispatch.CmdBeginRenderingKHR});
	aliasCmd(std::array{
		&dev.dispatch.CmdEndRendering,
		&dev.dispatch.CmdEndRenderingKHR});
	// VK_KHR_maintenance3, vulkan 1.1
	aliasCmd(std::array{
		&dev.dispatch.GetDescriptorSetLayoutSupport,
		&dev.dispatch.GetDescriptorSetLayoutSupportKHR});

	// find vkSetDeviceLoaderData callback
	if(standalone) {
		dev.setDeviceLoaderData = SetDeviceLoaderDataNOOP;
	} else {
		auto* loaderData = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*ci);
		while(loaderData && loaderData->function != VK_LOADER_DATA_CALLBACK) {
			loaderData = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*loaderData);
		}

		dlg_assert(loaderData);
		dev.setDeviceLoaderData = loaderData->u.pfnSetDeviceLoaderData;
	}

	// Get device queues
	for(auto i = 0u; i < queueCreateInfos.size(); ++i) {
		auto& qi = queueCreateInfos[i];
		dev.usedQueueFamilyIndices.push_back(qi.queueFamilyIndex);

		for(auto j = 0u; j < qi.queueCount; ++j) {
			auto& q = *dev.queues.emplace_back(std::make_unique<Queue>());
			q.dev = &dev;
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

#ifdef VIL_WITH_SWA
				// TODO: See https://github.com/nyorain/vil/issues/21.
				// We should instead query a suitable presentQueue at least
				// with the non-surface-specific query functions, via swa.
				// We just guess right now that the graphics queue has a good
				// chance of allowing presenting.
				if(window) {
					dlg_assert(!window->presentQueue);
					window->presentQueue = &q;
				}
#endif // VIL_WITH_SWA
			}

			if(dev.timelineSemaphores) {
				VkSemaphoreCreateInfo sci {};
				VkSemaphoreTypeCreateInfo tsci {};
				tsci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
				tsci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
				tsci.initialValue = 0u;
				sci.pNext = &tsci;

				sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
				VK_CHECK(dev.dispatch.CreateSemaphore(dev.handle, &sci, nullptr,
					&q.submissionSemaphore));
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

	// init static samplers
	VkSamplerCreateInfo sci {};
	sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sci.magFilter = VK_FILTER_NEAREST;
	sci.minFilter = VK_FILTER_NEAREST;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.minLod = -1000;
	sci.maxLod = 1000;
	sci.maxAnisotropy = 1.0f;
	VK_CHECK(dev.dispatch.CreateSampler(dev.handle, &sci, nullptr, &dev.nearestSampler));
	nameHandle(dev, dev.nearestSampler, "nearest");

	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	VK_CHECK(dev.dispatch.CreateSampler(dev.handle, &sci, nullptr, &dev.linearSampler));
	nameHandle(dev, dev.linearSampler, "linear");

	// descriptor pool
	// TODO: proper resource management. We now have
	std::array<VkDescriptorPoolSize, 3> poolSizes;
	poolSizes[0].descriptorCount = 500u;
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

	poolSizes[1].descriptorCount = 10u;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

	poolSizes[2].descriptorCount = 500u;
	poolSizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

	VkDescriptorPoolCreateInfo dpci {};
	dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpci.poolSizeCount = poolSizes.size();
	dpci.pPoolSizes = poolSizes.data();
	dpci.maxSets = 500u;
	dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	VK_CHECK(dev.dispatch.CreateDescriptorPool(dev.handle, &dpci, nullptr, &dev.dsPool));
	nameHandle(dev, dev.dsPool, "Device:dsPool");

	// init command hook
	dev.commandHook = std::make_unique<CommandHook>(dev);

#ifdef VIL_WITH_SWA
	if(window) {
		dlg_assert(window->presentQueue);
		dev.window = std::move(window);

		if(standaloneMode) {
			dev.window->dev = &dev;
			dev.window->doInitSwapchain();
		}
	}
#endif // VIL_WITH_SWA

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

	PFN_vkDestroyDevice pfnDestroyDev;
	VkDevice handle;

	if(HandleDesc<VkDevice>::wrap) {
		auto& wrapped = unwrapWrapped(vkDevice);
		auto& dev = wrapped.obj();

		// destroy our logical device before we forward the call
		handle = dev.handle;
		pfnDestroyDev = dev.dispatch.DestroyDevice;
		delete &wrapped;
	} else {
		auto devd = moveData<Device>(vkDevice);
		dlg_assert(devd);

		// destroy our logical device before we forward the call
		handle = devd->handle;
		pfnDestroyDev = devd->dispatch.DestroyDevice;
		devd.reset();
	}

	eraseDeviceFromLoaderMap(handle);

	dlg_assertm(DebugStats::get().aliveRecords == 0u,
		"{}", DebugStats::get().aliveRecords);
	pfnDestroyDev(handle, alloc);
}

// util
void notifyApiHandleDestroyedLocked(Device& dev, Handle& handle, VkObjectType type) {
	assertOwned(dev.mutex);
	if(dev.guiLocked()) {
		dev.guiLocked()->apiHandleDestroyed(handle, type);
	}
}

void notifyMemoryResourceInvalidatedLocked(Device& dev, MemoryResource& res) {
	assertOwned(dev.mutex);
	if(dev.guiLocked()) {
		dev.guiLocked()->memoryResourceInvalidated(res);
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
		return a < b;
	}

	return a->deviceAddress < b->deviceAddress;
}

bool Device::BufferAddressCmp::operator()(VkDeviceAddress a, const Buffer* b) const {
	dlg_assert(b && b->deviceAddress);
	return a < b->deviceAddress;
}

bool Device::BufferAddressCmp::operator()(const Buffer* a, VkDeviceAddress b) const {
	dlg_assert(a && a->deviceAddress);
	return a->deviceAddress < b;
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

// This delayed initialization is needed because we cannot
// call vulkan functions (such as CreateSwapchain) inside window
// creation, where we'd ideally initialize the window.
// https://github.com/KhronosGroup/Vulkan-Loader/pull/1049 broke
// this. We now have to make sure to make it later on.
void checkInitWindow(Device& dev) {
#ifdef VIL_WITH_SWA
	if(dev.window && !dev.window->dev) {
		dev.window->createWindow(*dev.ini);
		dev.window->initDevice(dev);
	}
#endif // VIL_WITH_SWA
}

void onDeviceLost(Device& dev) {
	dlg_error("device lost");

	if(!dev.extDeviceFault) {
		return;
	}

	VkDeviceFaultCountsEXT faultCounts {};
	faultCounts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
	dev.dispatch.GetDeviceFaultInfoEXT(dev.handle, &faultCounts, nullptr);

	std::vector<VkDeviceFaultAddressInfoEXT> addressInfos(faultCounts.addressInfoCount);
	std::vector<VkDeviceFaultVendorInfoEXT> vendorInfos(faultCounts.vendorInfoCount);
	std::vector<std::byte> vendorBinary(faultCounts.vendorBinarySize);

	VkDeviceFaultInfoEXT faultInfo {};
	faultInfo.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
	faultInfo.pAddressInfos = addressInfos.data();
	faultInfo.pVendorBinaryData = vendorBinary.data();
	faultInfo.pVendorInfos = vendorInfos.data();

	dev.dispatch.GetDeviceFaultInfoEXT(dev.handle, &faultCounts, &faultInfo);

	dlg_error("  description: {}", faultInfo.description);

	dlg_error("  {} addresses", faultCounts.addressInfoCount);
	for(auto& address : addressInfos) {
		dlg_error("  >> type {}, address {}", address.addressType, address.reportedAddress);
	}

	dlg_error("  {} vendorInfos", faultCounts.vendorInfoCount);
	for(auto& info : vendorInfos) {
		dlg_error("  >> {} {}, {}", info.vendorFaultCode, info.vendorFaultData, info.description);
	}

	dlg_error("  {} bytes of vendor specific data", faultCounts.vendorBinarySize);
}

// NOTE: doesn't really belong here
thread_local ThreadContext ThreadContext::instance;

} // namespace vil
