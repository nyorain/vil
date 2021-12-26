#define BUGGED_NO_MAIN

#include "../bugged.hpp"
#include "common.hpp"
#include <vk/vulkan.h>
#include <stdlib.h>
#include <dlg/dlg.hpp>

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

VulkanSetup gSetup;

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

static VkBool32 VKAPI_PTR messengerCallback(
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
	setenv("VK_ICD_FILENAMES", VIL_MOCK_ICD_FILE, 1);
	setenv("VK_LAYER_PATH", VIL_LAYER_PATH "/:/usr/share/vulkan/explicit_layer.d/", 1);

	dlg_trace("vk_layer_path: {}", getenv("VK_LAYER_PATH"));
	dlg_trace("vk_icd_filenames: {}", getenv("VK_ICD_FILENAMES"));

	// TODO: currently needed on CI to work around old validation layers
	// timeline semaphore bug
	setenv("VIL_TIMELINE_SEMAPHORES", "0", 1);

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

	dlg_trace("Creating instance");

	auto layers = std::array {
		"VK_LAYER_live_introspection",
		"VK_LAYER_KHRONOS_validation",
	};

	auto exts = std::array {
		VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
	};

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

	u32 qfam {0xFFFFFFFFu};
	for(auto i = 0u; i < qprops.size(); ++i) {
		if(qprops[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
			qfam = i;
			break;
		}
	}

	dlg_assert(qfam != 0xFFFFFFFFu);

	VkPhysicalDeviceFeatures features {};

	VkPhysicalDeviceVulkan11Features features11 {};
	features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;

	// NOTE: mock driver doesn't advertise support for 1.2
	// maybe enable it in fork? would be really useful for testing
	// VkPhysicalDeviceVulkan12Features features12 {};
	// features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	// features12.bufferDeviceAddress = true;

	const float prio1 = 1.f;
	VkDeviceQueueCreateInfo qci {};
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueCount = 1u;
	qci.queueFamilyIndex = qfam;
	qci.pQueuePriorities = &prio1;

	VkDeviceCreateInfo dci {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.pQueueCreateInfos = &qci;
	dci.queueCreateInfoCount = 1u;
	dci.pEnabledFeatures = &features;

	dci.pNext = &features11;
	// features11.pNext = &features12;

	VkDevice dev;
	VK_CHECK(vkCreateDevice(phdev, &dci, nullptr, &dev));

	VkQueue queue;
	vkGetDeviceQueue(dev, qfam, 0u, &queue);

	// store in setup
	gSetup.ini = ini;
	gSetup.dev = dev;
	gSetup.phdev = phdev;
	gSetup.qfam = qfam;
	gSetup.queue = queue;

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
	vilLoadSym(getErrorWarningCount);
	dlg_assert_or(getErrorWarningCount, return -2);
	auto vilWarnErrorCount = getErrorWarningCount();

	// teardown
	vkDestroyDevice(dev, nullptr);
	vkDestroyInstance(ini, nullptr);

	// Add error count
	// This includes warnings/errors by the validation layer
	dlg_trace("own errors/warnings: {}", dlgErrors + dlgWarnings);
	ret += dlgErrors;

	// Add warnings/errors that happened inside vil
	dlg_trace("vil errors/warnings: {}", vilWarnErrorCount);
	ret += vilWarnErrorCount;

	return ret;
}
