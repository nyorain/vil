#include <layer.hpp>
#include <device.hpp>
#include <util/export.hpp>
#include <util/dl.hpp>
#include <gui/gui.hpp>
#include <swa/swa.h>
#include <cstdlib>

using namespace vil;

VkBool32 VKAPI_PTR messengerCallback(
		VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		VkDebugUtilsMessageTypeFlagsEXT type,
		const VkDebugUtilsMessengerCallbackDataEXT* pData,
		void* userData) {
	(void) type;
	(void) userData;

	auto& data = *pData;

	// check if message is ignored
	std::vector<std::string> ignore = {};
	auto ig = std::find(ignore.begin(), ignore.end(), data.pMessageIdName);
	if(ig != ignore.end()) {
		return false;
	}

	auto level = dlg_level_trace;
	switch(severity) {
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
			level = dlg_level_error;
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
			level = dlg_level_warn;
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
			level = dlg_level_info;
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
			level = dlg_level_debug;
			break;
		default:
			break;
	}

	dlg_tags("DebugCallback");
	dlg_log(level, "{} ({})", data.pMessage, data.pMessageIdName);

	for(auto i = 0u; i < data.objectCount; ++i) {
		auto& obj = data.pObjects[i];
		if(obj.pObjectName) {
			dlg_log(level, "    involving '{}'", obj.pObjectName);
		}
	}

	for(auto i = 0u; i < data.queueLabelCount; ++i) {
		auto& label = data.pQueueLabels[i];
		auto name = label.pLabelName ? label.pLabelName : "<unnamed>";
		dlg_log(level, "    queue label '{}'", name);
	}

	for(auto i = 0u; i < data.cmdBufLabelCount; ++i) {
		auto& label = data.pCmdBufLabels[i];
		auto name = label.pLabelName ? label.pLabelName : "<unnamed>";
		dlg_log(level, "    cmd label '{}'", name);
	}

	return false;
}

extern "C" VIL_EXPORT int vil_showImageViewer(int argc, const char** argv) {
#if defined(_WIN32) || defined(__CYGWIN__)
	auto libvulkan = vil::dlopen("vulkan.dll");
#else // Unix
	auto libvulkan = vil::dlopen("libvulkan.so");
#endif

	auto pfnGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr) vil::dlsym(
		libvulkan, "vkGetInstanceProcAddr");
	dlg_assert(pfnGetInstanceProcAddr);

	auto layers = std::array {
		"VK_LAYER_KHRONOS_validation",
	};

	auto exts = std::vector {
		VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
	};

#define LOAD_INSTANCE_PROC(ini, name) \
	auto pfn##name = (PFN_vk##name) pfnGetInstanceProcAddr(ini, "vk" #name)

	{
		auto foundDebugUtils = false;
		auto foundSurface = false;
		auto foundPlatformExt = false;
		LOAD_INSTANCE_PROC(VK_NULL_HANDLE, EnumerateInstanceExtensionProperties);

		u32 extCount = 0u;
		VK_CHECK(pfnEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr));
		std::vector<VkExtensionProperties> extProps(extCount);
		VK_CHECK(pfnEnumerateInstanceExtensionProperties(nullptr, &extCount, extProps.data()));

		for(auto& ext : extProps) {
			auto name = ext.extensionName;
			dlg_info("Instance ext: {}, version {}", name, ext.specVersion);

			if(std::strcmp(name, VK_KHR_SURFACE_EXTENSION_NAME) == 0) {
				exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
				foundSurface = true;
			} else if(std::strcmp(name, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
				foundDebugUtils = true;
			} else if(std::strcmp(name, "VK_KHR_wayland_surface") == 0) {
				exts.push_back("VK_KHR_wayland_surface");
				foundPlatformExt = true;
			} else if(std::strcmp(name, "VK_KHR_win32_surface") == 0) {
				exts.push_back("VK_KHR_win32_surface");
				foundPlatformExt = true;
			} else if(std::strcmp(name, "VK_KHR_xcb_surface") == 0) {
				exts.push_back("VK_KHR_xcb_surface");
				foundPlatformExt = true;
			}
		}

		dlg_assert(foundDebugUtils);
		dlg_assert(foundSurface);
		dlg_assert(foundPlatformExt);
	}

	VkApplicationInfo appInfo {};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.apiVersion = VK_API_VERSION_1_2;

	VkInstanceCreateInfo ici {};
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.ppEnabledLayerNames = layers.data();
	ici.enabledLayerCount = layers.size();
	ici.ppEnabledExtensionNames = exts.data();
	ici.enabledExtensionCount = exts.size();
	ici.pApplicationInfo = &appInfo;

	VkDebugUtilsMessengerCreateInfoEXT dumci {};
	dumci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	dumci.messageType =
		VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
	dumci.messageSeverity =
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
	dumci.pfnUserCallback = messengerCallback;
	ici.pNext = &dumci;

	StandaloneInstanceInfo sii {};
	sii.getProcAddr = pfnGetInstanceProcAddr;

	VkInstance ini;
	VK_CHECK(vil::doCreateInstance(&ici, nullptr, &ini, &sii));

	LOAD_INSTANCE_PROC(ini, CreateDebugUtilsMessengerEXT);
	LOAD_INSTANCE_PROC(ini, DestroyDebugUtilsMessengerEXT);
	VkDebugUtilsMessengerEXT debugMessenger;
	pfnCreateDebugUtilsMessengerEXT(ini, &dumci, nullptr, &debugMessenger);

	// create device
	dlg_trace("Creating device");

	LOAD_INSTANCE_PROC(ini, EnumeratePhysicalDevices);
	LOAD_INSTANCE_PROC(ini, GetPhysicalDeviceQueueFamilyProperties);
	LOAD_INSTANCE_PROC(ini, GetPhysicalDeviceFeatures2);

	// TODO
	// LOAD_INSTANCE_PROC(ini, GetDeviceProcAddr);
	auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr) vil::dlsym(
		libvulkan, "vkGetDeviceProcAddr");
	dlg_assert(pfnGetInstanceProcAddr);

	u32 phdevCount = 10u;
	VkPhysicalDevice phdevs[10];
	VK_CHECK(pfnEnumeratePhysicalDevices(ini, &phdevCount, phdevs));

	if(phdevCount == 0u) {
		dlg_error("no physical device found");
		return -1;
	}

	auto phdev = phdevs[0];

	u32 qpropsCount = 0u;
	pfnGetPhysicalDeviceQueueFamilyProperties(phdev, &qpropsCount, nullptr);

	std::vector<VkQueueFamilyProperties> qprops(qpropsCount);
	pfnGetPhysicalDeviceQueueFamilyProperties(phdev, &qpropsCount, qprops.data());

	u32 qfamGfx {0xFFFFFFFFu};
	u32 qfamAsyncCompute {0xFFFFFFFFu};
	for(auto i = 0u; i < qprops.size(); ++i) {
		if(qprops[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
			qfamGfx = i;
		} else if(qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
			qfamAsyncCompute = i;
		}
	}

	dlg_assert(qfamGfx != 0xFFFFFFFFu);
	if(qfamAsyncCompute == 0xFFFFFFFFu) {
		qfamAsyncCompute = qfamGfx;
	}

	// query supported features
	VkPhysicalDeviceFeatures2 supFeatures {};
	VkPhysicalDeviceVulkan11Features sup11 {};
	VkPhysicalDeviceVulkan12Features sup12 {};

	supFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	sup11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
	sup12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

	supFeatures.pNext = &sup11;
	sup11.pNext = &sup12;

	pfnGetPhysicalDeviceFeatures2(phdev, &supFeatures);

	// activate features we might need
	VkPhysicalDeviceFeatures features {};

	VkPhysicalDeviceVulkan11Features features11 {};
	features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;

	VkPhysicalDeviceVulkan12Features features12 {};
	features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	if(sup12.bufferDeviceAddress) {
		features12.bufferDeviceAddress = true;
	}
	if(sup12.timelineSemaphore) {
		features12.timelineSemaphore = true;
	}

	const float prio1[] = {1.f, 1.f};
	VkDeviceQueueCreateInfo qcis[2] {};
	u32 qcCount = 1u;
	qcis[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qcis[0].queueCount = 1u;
	qcis[0].queueFamilyIndex = qfamGfx;
	qcis[0].pQueuePriorities = prio1;

	if(qfamAsyncCompute != qfamGfx) {
		++qcCount;
		qcis[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		qcis[1].queueCount = 1u;
		qcis[1].queueFamilyIndex = qfamAsyncCompute;
		qcis[1].pQueuePriorities = prio1;
	} else {
		++qcis[0].queueCount;
	}

	auto devExts = std::vector {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	};

	VkDeviceCreateInfo dci {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.pQueueCreateInfos = qcis;
	dci.queueCreateInfoCount = qcCount;
	dci.pEnabledFeatures = &features;
	dci.enabledExtensionCount = devExts.size();
	dci.ppEnabledExtensionNames = devExts.data();

	dci.pNext = &features11;
	features11.pNext = &features12;

	StandaloneDeviceInfo sdi {};
	sdi.getInstanceProcAddr = pfnGetInstanceProcAddr;
	sdi.getDeviceProcAddr = pfnGetDeviceProcAddr;

	VkDevice dev;
	VK_CHECK(vil::doCreateDevice(phdev, &dci, nullptr, &dev, &sdi));

	// at this point, there should be an instance and device
	// NOTE: we don't strictly need the mutex here as long as integration
	// test is single-threaded but keep it for good practice.
	vil::Device* vilDev {};

	{
		std::lock_guard lock(vil::dataMutex);
		dlg_assert(!vil::devByLoaderTable.empty());
		vilDev = vil::devByLoaderTable.begin()->second;
	}

	auto* gui = vilDev->gui();
	if(gui) {
		gui->mode_ = Gui::Mode::image;
	}

	// TODO: mainloop
	while(true) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	// shutdown
	vil::DestroyDevice(dev, nullptr);
	pfnDestroyDebugUtilsMessengerEXT(ini, debugMessenger, nullptr);
	vil::DestroyInstance(ini, nullptr);
	vil::dlclose(libvulkan);

	return 0;
}
