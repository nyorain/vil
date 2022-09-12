#include "../bugged.hpp"
#include "external.hpp"
#include <vk/vulkan.h>
#include <vk/dispatch_table_helper.h>
#include <stdlib.h>
#include <util/dlg.hpp>
#include <array>

#if defined(_WIN32) || defined(__CYGWIN__)
  // XXX NOTE ATTENTION: this header includes windows.h.
  // Just as a fair warning, this might break your application and will to live.
  // Nothing we can really do about it, we need the functions to load func pointers.
  // A possible workaround would be to move the vilLoadApi implementation
  // out of the header, into a separate file.
  #include <windows.h>
  #define VIL_LIB_NAME "VkLayer_live_introspection.dll"
#else
  #include <sys/stat.h>
  #include <string.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <ctype.h>
  #include <signal.h>

  #include <dlfcn.h>
  #define VIL_LIB_NAME "libVkLayer_live_introspection.so"
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
	#define VIL_IMPORT __declspec(dllimport)
#else
	#define VIL_IMPORT
#endif

using PFN_vil_getErrorWarningCount = int(*)();
using PFN_vil_runInternalIntegrationTets = int(*)(
		VkInstance ini, PFN_vkGetInstanceProcAddr);

#ifdef _WIN32

int setenv(const char *name, const char *value, int overwrite) {
    int errcode = 0;
    if(!overwrite) {
        size_t envsize = 0;
        errcode = getenv_s(&envsize, NULL, 0, name);
        if(errcode || envsize) return errcode;
    }
    return _putenv_s(name, value);
}

#endif // _WIN32

Setup gSetup;

unsigned dlgErrors = 0;
unsigned dlgWarnings = 0;

#ifdef WIN32

bool debuggerAttached() {
	return ::IsDebuggerPresent();
}

void debugBreak() {
	::DebugBreak();
}

#else // WIN32

bool debuggerAttached() {
    char buf[4096];

    const int statusFD = ::open("/proc/self/status", O_RDONLY);
    if(statusFD == -1) {
        return false;
	}

    const ssize_t nr = ::read(statusFD, buf, sizeof(buf) - 1);
    if(nr <= 0) {
		::close(statusFD);
        return false;
	}

    buf[nr] = '\0';

    constexpr char tracerPidString[] = "TracerPid:";
    const auto tracerPidPtr = ::strstr(buf, tracerPidString);

    if(!tracerPidPtr) {
		::close(statusFD);
        return false;
	}

	const char* it = tracerPidPtr + sizeof(tracerPidString) - 1;
    while(it < buf + nr) {
        if(!::isspace(*it)) {
			::close(statusFD);
            return ::isdigit(*it) != 0 && *it != '0';
		}

		++it;
    }

	::close(statusFD);
    return false;
}

void debugBreak() {
	::raise(SIGINT);
}

#endif

void dlgHandler(const struct dlg_origin* origin, const char* string, void* data) {
	if(origin->level == dlg_level_error) {
		++dlgErrors;
	} else if(origin->level == dlg_level_warn) {
		++dlgWarnings;
	}

	dlg_default_output(origin, string, data);

	if(origin->level >= dlg_level_warn && debuggerAttached()) {
		debugBreak();
	}
}

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

int main() {
	dlg_set_handler(dlgHandler, nullptr);

	// set null driver
	// TODO: allow to configure this via options.
	// Allowing to run this via normal driver might be interesting for development
	setenv("VK_ICD_FILENAMES", VIL_MOCK_ICD_FILE, 1);
	// TODO: don't hardcode the vulkan layer path here. Not sure how
	// to properly retrieve it though, need to handle it per-platform.
	// setenv("VK_LAYER_PATH", VIL_LAYER_PATH "/:/usr/share/vulkan/explicit_layer.d/", 1);

	std::string layerPath = VIL_LAYER_PATH;
#ifdef _WIN32
	layerPath += ";"; // seperator
	auto vkSDKPath = getenv("VULKAN_SDK");
	layerPath += vkSDKPath;
	layerPath += "\\Bin";
#else // _WIN32
	layerPath += "/:/usr/share/vulkan/explicit_layer.d/";
#endif // _WIN32

	setenv("VK_LAYER_PATH", layerPath.c_str(), 1);

	dlg_trace("vk_layer_path: {}", getenv("VK_LAYER_PATH"));
	dlg_trace("vk_icd_filenames: {}", getenv("VK_ICD_FILENAMES"));

	// TODO: currently needed on CI to work around old validation layers
	// timeline semaphore bug
	// setenv("VIL_TIMELINE_SEMAPHORES", "0", 1);
	setenv("VIL_DLG_HANDLER", "1", 1);

	// enumerate layers
	{
		u32 layerCount = 0u;
		VK_CHECK(vkEnumerateInstanceLayerProperties(&layerCount, nullptr));
		std::vector<VkLayerProperties> layerProps(layerCount);
		VK_CHECK(vkEnumerateInstanceLayerProperties(&layerCount, layerProps.data()));

		for(auto& layer : layerProps) {
			dlg_info("Instance layer: {}, spec version {}.{}.{}", layer.layerName,
				VK_VERSION_MAJOR(layer.specVersion),
				VK_VERSION_MINOR(layer.specVersion),
				VK_VERSION_PATCH(layer.specVersion));
		}
	}

	// enumerate instance extensions
	auto foundDebugUtils = false;
	auto foundHeadlessSurfaceExt = false;
	{
		auto foundSurfaceExt = false;

		u32 extCount = 0u;
		VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr));
		std::vector<VkExtensionProperties> extProps(extCount);
		VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &extCount, extProps.data()));

		for(auto& ext : extProps) {
			auto name = ext.extensionName;
			dlg_info("Instance ext: {}, version {}", name, ext.specVersion);
			if(std::strcmp(name, VK_KHR_SURFACE_EXTENSION_NAME) == 0) {
				foundSurfaceExt = true;
			} else if(std::strcmp(name, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME) == 0) {
				foundHeadlessSurfaceExt = true;
			} else if(std::strcmp(name, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
				foundDebugUtils = true;
			}
		}

		foundHeadlessSurfaceExt = foundHeadlessSurfaceExt && foundSurfaceExt;
	}

	dlg_assert(foundDebugUtils);
	dlg_trace("Creating instance");

	auto layers = std::array {
		"VK_LAYER_live_introspection",
		"VK_LAYER_KHRONOS_validation",
	};

	auto exts = std::vector {
		VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
	};

	if(foundHeadlessSurfaceExt) {
		exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
		exts.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
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

	VkInstance ini;
	VK_CHECK(vkCreateInstance(&ici, nullptr, &ini));

	// create debug messenger
	auto fpCreateDebugUtilsMessengerEXT =
		(PFN_vkCreateDebugUtilsMessengerEXT)
		vkGetInstanceProcAddr(ini, "vkCreateDebugUtilsMessengerEXT");
	auto fpDestroyDebugUtilsMessengerEXT =
		(PFN_vkDestroyDebugUtilsMessengerEXT)
		vkGetInstanceProcAddr(ini, "vkDestroyDebugUtilsMessengerEXT");
	VkDebugUtilsMessengerEXT debugMessenger;
	fpCreateDebugUtilsMessengerEXT(ini, &dumci, nullptr, &debugMessenger);

	gSetup.ini = ini;
	layer_init_instance_dispatch_table(gSetup.ini, &gSetup.iniDispatch,
		&vkGetInstanceProcAddr);

	// init device
	dlg_trace("Creating device");

	u32 phdevCount = 1u;
	VkPhysicalDevice phdev;
	VK_CHECK(vkEnumeratePhysicalDevices(ini, &phdevCount, &phdev));

	if(phdevCount == 0u) {
		dlg_error("no physical device found");
		return -1;
	}

	u32 qpropsCount = 0u;
	vkGetPhysicalDeviceQueueFamilyProperties(phdev, &qpropsCount, nullptr);

	std::vector<VkQueueFamilyProperties> qprops(qpropsCount);
	vkGetPhysicalDeviceQueueFamilyProperties(phdev, &qpropsCount, qprops.data());

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

	vkGetPhysicalDeviceFeatures2(phdev, &supFeatures);

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

	VkDevice dev;
	VK_CHECK(vkCreateDevice(phdev, &dci, nullptr, &dev));

	VkQueue queueGfx, queueCompute;
	vkGetDeviceQueue(dev, qfamGfx, 0u, &queueGfx);
	if(qfamGfx == qfamAsyncCompute) {
		vkGetDeviceQueue(dev, qfamGfx, 1u, &queueCompute);
	} else {
		vkGetDeviceQueue(dev, qfamAsyncCompute, 0u, &queueCompute);
	}

	// store in setup
	gSetup.dev = dev;
	gSetup.phdev = phdev;
	gSetup.qfam = qfamGfx;
	gSetup.qfam2 = qfamAsyncCompute;
	gSetup.queue = queueGfx;
	gSetup.queue2 = queueCompute;
	layer_init_device_dispatch_table(gSetup.dev, &gSetup.dispatch, &vkGetDeviceProcAddr);

	// run tests
	auto pattern = nullptr;
	auto ret = vil::bugged::Testing::get().run(pattern);

	// TODO: cleanup. Taken from vil_api.h
#if defined(_WIN32) || defined(__CYGWIN__)
	HMODULE handle = GetModuleHandleA(VIL_LIB_NAME);

	// We don't have to call FreeLibrary since GetModuleHandle does not increase ref count
	#define vilCloseLib()
	#define vilLoadSym(procName) *(FARPROC*) (&procName) = GetProcAddress(handle, "vil_" #procName)
#else
	void* handle = dlopen(VIL_LIB_NAME, RTLD_NOLOAD | RTLD_LAZY);

	// We have to call dlclose since our dlopen increases the reference count.
	#define vilCloseLib() dlclose(handle)
	#define vilLoadSym(procName) *(void**) &(procName) = dlsym(handle, "vil_" #procName)
#endif

	PFN_vil_getErrorWarningCount getErrorWarningCount;
	PFN_vil_runInternalIntegrationTets runInternalIntegrationTests;

	vilLoadSym(getErrorWarningCount);
	vilLoadSym(runInternalIntegrationTests);

	dlg_assert_or(getErrorWarningCount, return -2);
	dlg_assert_or(runInternalIntegrationTests, return -2);

	dlg_trace("Running internal integration tests... ");

	auto failCount = runInternalIntegrationTests(ini, &vkGetInstanceProcAddr);
	ret += failCount;
	dlg_trace(">> Done. Failures: {}", failCount);

	auto vilWarnErrorCount = getErrorWarningCount();

	vilCloseLib();

	// teardown
	vkDestroyDevice(dev, nullptr);
	fpDestroyDebugUtilsMessengerEXT(ini, debugMessenger, nullptr);

#ifndef _WIN32
	// destroying the instance leads to a crash *after* main on windows (msvc).
	// I have no idea why, seems terribly broken. This also happens without our
	// custom icd and without vil so probably a loader error. Ugh.
	// So we just leak this on windows, yay.
	vkDestroyInstance(ini, nullptr);
#endif // _WIN32

	// Add error count
	// This includes warnings/errors by the validation layer
	dlg_trace("own errors/warnings: {}", dlgErrors + dlgWarnings);
	ret += dlgErrors;

	// Add warnings/errors that happened inside vil
	dlg_trace("vil errors/warnings: {}", vilWarnErrorCount);
	ret += vilWarnErrorCount;

	return ret;
}
