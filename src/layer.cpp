#include <layer.hpp>
#include <wrap.hpp>
#include <data.hpp>
#include <swapchain.hpp>
#include <image.hpp>
#include <buffer.hpp>
#include <queryPool.hpp>
#include <sync.hpp>
#include <rp.hpp>
#include <cb.hpp>
#include <memory.hpp>
#include <pipe.hpp>
#include <shader.hpp>
#include <ds.hpp>
#include <platform.hpp>
#include <queue.hpp>
#include <overlay.hpp>
#include <accelStruct.hpp>
#include <threadContext.hpp>
#include <util/util.hpp>
#include <util/export.hpp>
#include <util/profiling.hpp>
#include <util/dlg.hpp>
#include <vkutil/enumString.hpp>

#ifdef VIL_WITH_WAYLAND
  #include <wayland.hpp>
#endif // VIL_WITH_WAYLAND

#ifdef VIL_WITH_X11
  #include <xlib.hpp>
#endif // VIL_WITH_X11

#ifdef VIL_WITH_WIN32
  #include <win32.hpp>
#endif // VIL_WITH_WIN32

#include <vk/dispatch_table_helper.h>
#include <csignal>

#include <vil_api.h>

namespace vil {

// Util
static auto dlgWarnErrorCount = 0u;
static auto breakOnError = false;
static auto dlgMinLevel = dlg_level_trace;

// TODO: doesn't belong here
std::mutex ThreadContext::mutex_;
std::vector<ThreadContext*> ThreadContext::contexts_;

void dlgHandler(const struct dlg_origin* origin, const char* string, void* data) {
	// (void) string;
	// (void) data;
	// (void) origin;

#ifndef DLG_DISABLE
	if(origin->level < dlgMinLevel) {
		return;
	}

	dlg_default_output(origin, string, data);

	if(origin->level >= dlg_level_warn) {
		++dlgWarnErrorCount;
	}

	// break
	if(origin->level >= dlg_level_error && breakOnError) {
		#ifdef _MSC_VER
			DebugBreak();
		#else
			// std::raise(SIGABRT);
			std::raise(SIGTRAP);
		#endif
	}
#endif // DLG_DISABLE
}

std::array<unsigned int, 3> apiVersion(uint32_t v) {
	return {
		VK_VERSION_MAJOR(v),
		VK_VERSION_MINOR(v),
		VK_VERSION_PATCH(v)
	};
}

Instance::~Instance() {
}

void initSettings() {
	auto enableWrapping = checkEnvBinary("VIL_WRAP", true);

	auto checkSet = [&](auto& enable, const char* var) {
		enable = checkEnvBinary(var, enableWrapping);
	};

	checkSet(HandleDesc<VkCommandBuffer>::wrap, "VIL_WRAP_COMMAND_BUFFER");
	checkSet(HandleDesc<VkDescriptorSet>::wrap, "VIL_WRAP_DESCRIPTOR_SET");
	checkSet(HandleDesc<VkDescriptorSetLayout>::wrap, "VIL_WRAP_DESCRIPTOR_SET_LAYOUT");
	checkSet(HandleDesc<VkPipelineLayout>::wrap, "VIL_WRAP_PIPELINE_LAYOUT");
	checkSet(HandleDesc<VkCommandPool>::wrap, "VIL_WRAP_COMMAND_POOL");
	checkSet(HandleDesc<VkDescriptorUpdateTemplate>::wrap, "VIL_WRAP_BUFFER_DESCRIPTOR_UPDATE_TEMPLATE");
	checkSet(HandleDesc<VkImage>::wrap, "VIL_WRAP_IMAGE");
	checkSet(HandleDesc<VkPipeline>::wrap, "VIL_WRAP_PIPELINE");

	// These are always wrapped now. Needed for atomic descriptor set updates.
	// checkSet(HandleDesc<VkBufferView>::wrap, "VIL_WRAP_BUFFER_VIEW");
	// checkSet(HandleDesc<VkImageView>::wrap, "VIL_WRAP_IMAGE_VIEW");
	// checkSet(HandleDesc<VkBuffer>::wrap, "VIL_WRAP_BUFFER");
	// checkSet(HandleDesc<VkSampler>::wrap, "VIL_WRAP_SAMPLER");
	// checkSet(HandleDesc<VkSampler>::wrap, "VIL_WRAP_ACCELERATION_STRUCTURE");
}

#ifdef TRACY_MANUAL_LIFETIME
	// NOTE: strictly speaking, we'd need a mutex to do proper
	//   initialization. But creating multiple Instances from multiple threads
	//   at the same time is a weird and problematic corner case anyways
	std::atomic<unsigned> tracyRefCount {};
#endif // TRACY_MANUAL_LIFETIME

void initTracy() {
#ifdef TRACY_MANUAL_LIFETIME
	if(tracyRefCount.fetch_add(1u) == 0u) {
		dlg_trace("Starting tracy...");
		tracy::StartupProfiler();
		dlg_trace(">> done");
	}
#endif // TRACY_MANUAL_LIFETIME
}

void shutdownTracy() {
#ifdef TRACY_MANUAL_LIFETIME
 	if(tracyRefCount.fetch_sub(1u) == 1u) {
		// TODO: hacky af
		// make sure to cleaer threadcontext memory before tracy
		// is shut down.
		{
			std::lock_guard lock(vil::ThreadContext::mutex_);
			for(auto* tc : vil::ThreadContext::contexts_) {
				dlg_assert(tc->linalloc_.empty());
				tc->linalloc_.release();
			}
		}

 		tracy::ShutdownProfiler();
 	}
#endif // TRACY_MANUAL_LIFETIME
}

VkResult VKAPI_PTR SetInstanceLoaderDataNOOP(VkInstance, void*) {
	// noop
	return VK_SUCCESS;
}

// Instance
VkResult doCreateInstance(
		const VkInstanceCreateInfo* ci,
		const VkAllocationCallbacks* alloc,
		VkInstance* pInstance,
		StandaloneInstanceInfo* standalone) {
	initTracy();

	// We use a static version of dlg so this shouldn't be an issue.
	// TODO: check if it's really ok on all platforms.
	//   NOPE, THIS SETS THE GLOBAL HANDLER ON LINUX UGH
	//   We need this for tests tho
	// TODO: maybe control via environment variable whether we do this?
	//   When a vil::Gui was created, we could show all output there.
	// TODO: remove/find real solution for AllocConsole on windows
	//  maybe control this via environment variable?
	//  On windows (with msvc), we could use DebugOutput.
	if(checkEnvBinary("VIL_BREAK_ON_ERROR", false)) {
		breakOnError = true;
	}

#ifndef DLG_DISABLE
	if(checkEnvBinary("VIL_DLG_HANDLER", false) || breakOnError) {
		if(const char* minLevel = std::getenv("VIL_MIN_LOG_LEVEL"); minLevel) {
			if(std::strcmp(minLevel, "trace")) dlgMinLevel = dlg_level_warn;
			else if(std::strcmp(minLevel, "debug")) dlgMinLevel = dlg_level_debug;
			else if(std::strcmp(minLevel, "info")) dlgMinLevel = dlg_level_info;
			else if(std::strcmp(minLevel, "warn")) dlgMinLevel = dlg_level_warn;
			else if(std::strcmp(minLevel, "error")) dlgMinLevel = dlg_level_error;
			else dlg_error("Invalid value for VIL_MIN_LOG_LEVEL: {}", minLevel);
		}

		dlg_set_handler(dlgHandler, nullptr);
		#ifdef _WIN32
			AllocConsole();
			dlg_trace("Allocated console. Creating vulkan instance");
		#endif // _WIN32
	}
#endif // DLG_DISABLE

	PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr {};
	VkLayerInstanceCreateInfo* mutLinkInfo {};
	if(standalone) {
		fpGetInstanceProcAddr = standalone->getProcAddr;
	} else {
		auto* linkInfo = findChainInfo<VkLayerInstanceCreateInfo, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO>(*ci);
		while(linkInfo && linkInfo->function != VK_LAYER_LINK_INFO) {
			linkInfo = findChainInfo<VkLayerInstanceCreateInfo, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO>(*linkInfo);
		}

		if(!linkInfo) {
			dlg_error("No linkInfo");
			return VK_ERROR_INITIALIZATION_FAILED;
		}

		fpGetInstanceProcAddr = linkInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;

		// Advance the link info for the next element on the chain
		mutLinkInfo = const_cast<VkLayerInstanceCreateInfo*>(linkInfo);
		mutLinkInfo->u.pLayerInfo = linkInfo->u.pLayerInfo->pNext;
	}

	dlg_assert(fpGetInstanceProcAddr);

	auto fpCreateInstance = (PFN_vkCreateInstance)fpGetInstanceProcAddr(nullptr, "vkCreateInstance");
	if(!fpCreateInstance) {
		dlg_error("could not load vkCreateInstance");
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	// Init instance data
	// NOTE: we cannot add extensions here, sadly.
	// See https://github.com/KhronosGroup/Vulkan-Loader/issues/51
	auto nci = *ci;

	const auto tryBumpVersion = checkEnvBinary("VIL_BUMP_API_VERSION", true);
	constexpr auto ourApiVersion = VK_API_VERSION_1_2;

	// NOTE: we can't call vkEnumerateInstanceVersion ourselves.
	// So we just trial-and-error to possibly bump up the version.
	// When instance creation fails just turn it down to original again.
	auto originalApiVersion = VK_API_VERSION_1_0;
	auto finalApiVersion = originalApiVersion;

	auto formatVersion = [](auto version) {
		return dlg::format("{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version));
	};

	VkApplicationInfo ourAppInfo {};
	if(nci.pApplicationInfo) {
		originalApiVersion = nci.pApplicationInfo->apiVersion;
		finalApiVersion = originalApiVersion;
		if(nci.pApplicationInfo->apiVersion < ourApiVersion && tryBumpVersion) {
			dlg_debug("Trying to manually bump instance apiVersion to 1.2 "
				"instead of {} provided by application",
				formatVersion(nci.pApplicationInfo->apiVersion));
			ourAppInfo = *nci.pApplicationInfo;
			ourAppInfo.apiVersion = ourApiVersion;
			nci.pApplicationInfo = &ourAppInfo;
			finalApiVersion = ourApiVersion;
		}
	} else if(tryBumpVersion) {
		dlg_debug("Trying to manually bump instance apiVersion to 1.2 (instead of default 1.0)");
		ourAppInfo.apiVersion = ourApiVersion;
		nci.pApplicationInfo = &ourAppInfo;
		finalApiVersion = ourApiVersion;
	}

	// == Create instance ==
	VkLayerInstanceCreateInfo linkInfoCopy {};
	if(mutLinkInfo) {
		linkInfoCopy = *mutLinkInfo;
	}

	VkResult result = fpCreateInstance(&nci, alloc, pInstance);
	if(result != VK_SUCCESS) {
		dlg_debug("vkCreateInstance failed: {}", vk::name(result));

		// in this case we didn't change it.
		if(ourApiVersion <= originalApiVersion) {
			return result;
		}

		// important mainly for vulkan validation layer tests
		if(result == VK_ERROR_VALIDATION_FAILED_EXT) {
			return result;
		}

		dlg_debug("Trying again with original api version");
		dlg_assert(nci.pApplicationInfo == &ourAppInfo);
		finalApiVersion = originalApiVersion;
		ourAppInfo.apiVersion = originalApiVersion;

		// NOTE: make sure to restore link info, might have been modified
		// on last tries
		if(mutLinkInfo) {
			*mutLinkInfo = linkInfoCopy;
		}

		result = fpCreateInstance(&nci, alloc, pInstance);

		if(result != VK_SUCCESS) {
			return result;
		}
	}

	initSettings();

	auto iniPtr = std::make_unique<Instance>();
	auto& ini = *iniPtr;

	insertData(*pInstance, iniPtr.release());
	ini.handle = *pInstance;

	auto extsBegin = ci->ppEnabledExtensionNames;
	auto extsEnd = ci->ppEnabledExtensionNames + ci->enabledExtensionCount;
	ini.extensions = {extsBegin, extsEnd};

	auto debugUtilsName = std::string_view(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	ini.debugUtilsEnabled = (std::find(extsBegin, extsEnd, debugUtilsName) != extsEnd);

	auto strOrEmpty = [](const char* str) {
		return str ? str : "";
	};

	ini.app.apiVersion = VK_API_VERSION_1_0;
	if(ci->pApplicationInfo) {
		ini.app.valid = true;
		ini.app.apiVersion = ci->pApplicationInfo->apiVersion;
		ini.app.version = ci->pApplicationInfo->applicationVersion;
		ini.app.name = strOrEmpty(ci->pApplicationInfo->pApplicationName);
		ini.app.engineName = strOrEmpty(ci->pApplicationInfo->pEngineName);
		ini.app.engineVersion = ci->pApplicationInfo->engineVersion;
	}

	ini.vulkan11 = (finalApiVersion >= VK_API_VERSION_1_1);
	ini.vulkan12 = (finalApiVersion >= VK_API_VERSION_1_2);

	layer_init_instance_dispatch_table(*pInstance, &ini.dispatch, fpGetInstanceProcAddr);

	// find vkSetInstanceLoaderData callback
	if(!standalone) {
		auto* loaderData = findChainInfo<VkLayerInstanceCreateInfo, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO>(*ci);
		while(loaderData && loaderData->function != VK_LOADER_DATA_CALLBACK) {
			loaderData = findChainInfo<VkLayerInstanceCreateInfo, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO>(*loaderData);
		}

		dlg_assert(loaderData);
		ini.setInstanceLoaderData = loaderData->u.pfnSetInstanceLoaderData;
	} else {
		ini.setInstanceLoaderData = SetInstanceLoaderDataNOOP;
	}

	// NOTE: not sure if this is needed actually.
	// Should do it for all commands that need it for now.
	// We are also doing this in device.
	aliasCmd(std::array{
		&ini.dispatch.GetPhysicalDeviceProperties2,
		&ini.dispatch.GetPhysicalDeviceProperties2KHR});
	aliasCmd(std::array{
		&ini.dispatch.GetPhysicalDeviceMemoryProperties2,
		&ini.dispatch.GetPhysicalDeviceMemoryProperties2KHR});
	aliasCmd(std::array{
		&ini.dispatch.GetPhysicalDeviceImageFormatProperties2,
		&ini.dispatch.GetPhysicalDeviceImageFormatProperties2KHR});
	aliasCmd(std::array{
		&ini.dispatch.GetPhysicalDeviceFormatProperties2,
		&ini.dispatch.GetPhysicalDeviceFormatProperties2KHR});
	aliasCmd(std::array{
		&ini.dispatch.GetPhysicalDeviceFeatures2,
		&ini.dispatch.GetPhysicalDeviceFeatures2KHR});

	// add instance data to all physical devices so we can retrieve
	// it in CreateDevice
	u32 phdevCount = 0;
	ini.dispatch.EnumeratePhysicalDevices(*pInstance, &phdevCount, nullptr);
	ini.phdevs.resize(phdevCount);
	ini.dispatch.EnumeratePhysicalDevices(*pInstance, &phdevCount, ini.phdevs.data());

	for(auto i = 0u; i < phdevCount; ++i) {
		insertData(ini.phdevs[i], &ini);
	}

	return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(
		const VkInstanceCreateInfo* ci,
		const VkAllocationCallbacks* alloc,
		VkInstance* pInstance) {
	return doCreateInstance(ci, alloc, pInstance);
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance ini, const VkAllocationCallbacks* alloc) {
	if(!ini) {
		return;
	}

	// destroy the instance
	{
		auto inid = moveData<Instance>(ini);
		dlg_assert(inid);

		// remove physical devices we inserted
		for(auto& phdev : inid->phdevs) {
			eraseData(phdev);
		}

		inid->dispatch.DestroyInstance(ini, alloc);
	}

	shutdownTracy();
}

// tmp test
void CmdCuLaunchKernelNVX(
		VkCommandBuffer                             commandBuffer,
		const VkCuLaunchInfoNVX* pLaunchInfo) {

	(void) commandBuffer;
	(void) pLaunchInfo;

	dlg_error("uffff wtf");
	// DebugBreak();
}

VkResult GetImageViewAddressNVX(
		VkDevice                                    device,
		VkImageView                                 imageView,
		VkImageViewAddressPropertiesNVX* pProperties) {
	(void) device;
	(void) imageView;
	(void) pProperties;

	dlg_error("uffff wtf");
	// DebugBreak();

	return VK_SUCCESS;
}

uint32_t GetImageViewHandleNVX(
		VkDevice                                    device,
		const VkImageViewHandleInfoNVX* pInfo) {
	(void) device;
	(void) pInfo;
	dlg_error("uffff wtf");
	// DebugBreak();

	return 0u;
}

struct HookedFunction {
	PFN_vkVoidFunction func {};
	bool device {}; // device-level function
	u32 version {}; // required vulkan version
	// TODO: we never need both fields i guess, just merge them into 'ext'?
	std::string_view iniExt {}; // name of extension that has to be enabled
	std::string_view devExt {}; // name of extension that has to be enabled
};

// We make sure our hooked functions match the type required by vulkan
// since everything below here is just wild function pointer casting.
#define FN_TC(fn, val) []{ \
	static_assert(std::is_same_v<decltype(&fn), PFN_vk ## fn>); \
	return val; \
}()

// For aliases we additionally make sure that original and alias function
// type matches. And we check that original and alias aren't the same (as
// that mistake can be done easily).
#define FN_TC_ALIAS(alias, fn, val) []{ \
	static_assert(std::string_view(#alias) != std::string_view(#fn), "Alias and original can't be same"); \
	using ImplF = decltype(&fn); \
	using Ref = PFN_vk ## fn; \
	using AliasRef = PFN_vk ## alias; \
	static_assert(std::is_same_v<ImplF, Ref>); \
	static_assert(std::is_same_v<AliasRef, Ref>); \
	return val; \
}()

#define VIL_INI_HOOK(fn, ver) {"vk" # fn, {(PFN_vkVoidFunction) fn, FN_TC(fn, false), ver, {}}}
#define VIL_INI_HOOK_EXT(fn, ext) {"vk" # fn, {(PFN_vkVoidFunction) fn, FN_TC(fn, false), VK_VERSION_1_0, ext}}

#define VIL_DEV_HOOK(fn, ver) {"vk" # fn, {(PFN_vkVoidFunction) fn, FN_TC(fn, true), ver, {}, {}}}
#define VIL_DEV_HOOK_EXT(fn, ext) {"vk" # fn, {(PFN_vkVoidFunction) fn, FN_TC(fn, true), VK_VERSION_1_0, {}, ext}}
#define VIL_DEV_HOOK_ALIAS(alias, fn, ext) {"vk" # alias, {(PFN_vkVoidFunction) fn, FN_TC_ALIAS(alias, fn, true), VK_VERSION_1_0, {}, ext}}

// NOTE: not sure about these, it seems applications can use KHR functions without
// enabling the extension when the function is in core? The vulkan samples do this
// at least. So we return them as well.
#define VIL_DEV_HOOK_ALIAS_CORE(alias, fn, ext) {"vk" # alias, {(PFN_vkVoidFunction) fn, FN_TC_ALIAS(alias, fn, true), VK_VERSION_1_0, {}, {}}}

static const std::unordered_map<std::string_view, HookedFunction> funcPtrTable {
	VIL_INI_HOOK(GetInstanceProcAddr, VK_API_VERSION_1_0),
	VIL_INI_HOOK(CreateInstance, VK_API_VERSION_1_0),
	VIL_INI_HOOK(DestroyInstance, VK_API_VERSION_1_0),

	VIL_DEV_HOOK(GetDeviceProcAddr, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CreateDevice, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroyDevice, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DeviceWaitIdle, VK_API_VERSION_1_0),

	// queue.hpp
	VIL_DEV_HOOK(QueueSubmit, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(QueueWaitIdle, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(QueueBindSparse, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(QueueSubmit2, VK_API_VERSION_1_3),
	VIL_DEV_HOOK_ALIAS_CORE(QueueSubmit2KHR, QueueSubmit2,
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME),

	// swapchain.hpp
	VIL_DEV_HOOK_EXT(CreateSwapchainKHR, VK_KHR_SWAPCHAIN_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(DestroySwapchainKHR, VK_KHR_SWAPCHAIN_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(AcquireNextImageKHR, VK_KHR_SWAPCHAIN_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(AcquireNextImage2KHR, VK_KHR_SWAPCHAIN_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(QueuePresentKHR, VK_KHR_SWAPCHAIN_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(GetSwapchainImagesKHR, VK_KHR_SWAPCHAIN_EXTENSION_NAME),

	// handle.hpp
	VIL_DEV_HOOK_EXT(SetDebugUtilsObjectNameEXT, VK_EXT_DEBUG_UTILS_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(SetDebugUtilsObjectTagEXT, VK_EXT_DEBUG_UTILS_EXTENSION_NAME),

#ifdef VIL_WITH_WAYLAND
	VIL_HOOK_EXT(CreateWaylandSurfaceKHR, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME),
#endif // VIL_WITH_WAYLAND

#ifdef VIL_WITH_X11
	VIL_INI_HOOK_EXT(CreateXlibSurfaceKHR, VK_KHR_XLIB_SURFACE_EXTENSION_NAME),
	VIL_INI_HOOK_EXT(CreateXcbSurfaceKHR, VK_KHR_XCB_SURFACE_EXTENSION_NAME),
#endif // VIL_WITH_X11

#ifdef VIL_WITH_WIN32
	// We do everything to not include the platform-specific header
	// VIL_INI_HOOK_EXT(CreateWin32SurfaceKHR, VK_KHR_WIN32_SURFACE_EXTENSION_NAME),
	{"vkCreateWin32SurfaceKHR", HookedFunction{(PFN_vkVoidFunction) CreateWin32SurfaceKHR, false, VK_VERSION_1_0, "VK_KHR_win32_surface"}},
#endif // VIL_WITH_WIN32

	VIL_INI_HOOK_EXT(DestroySurfaceKHR, VK_KHR_SURFACE_EXTENSION_NAME),

	// rp.hpp
	VIL_DEV_HOOK(CreateFramebuffer, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroyFramebuffer, VK_API_VERSION_1_0),

	VIL_DEV_HOOK(CreateRenderPass, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroyRenderPass, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CreateRenderPass2, VK_API_VERSION_1_2),
	VIL_DEV_HOOK_ALIAS_CORE(CreateRenderPass2KHR, CreateRenderPass2,
		VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME),

	// image.hpp
	VIL_DEV_HOOK(CreateImage, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroyImage, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(GetImageMemoryRequirements, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(GetImageSparseMemoryRequirements, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(GetImageSubresourceLayout, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(BindImageMemory, VK_API_VERSION_1_0),

	VIL_DEV_HOOK(BindImageMemory2, VK_API_VERSION_1_1),
	VIL_DEV_HOOK_ALIAS_CORE(BindImageMemory2KHR, BindImageMemory2,
		VK_KHR_BIND_MEMORY_2_EXTENSION_NAME),

	VIL_DEV_HOOK(GetImageMemoryRequirements2, VK_API_VERSION_1_1),
	VIL_DEV_HOOK_ALIAS_CORE(GetImageMemoryRequirements2KHR, GetImageMemoryRequirements2,
		VK_KHR_BIND_MEMORY_2_EXTENSION_NAME),

	VIL_DEV_HOOK_EXT(GetImageDrmFormatModifierPropertiesEXT,
		VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME),

	VIL_DEV_HOOK(CreateImageView, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroyImageView, VK_API_VERSION_1_0),

	VIL_DEV_HOOK(CreateSampler, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroySampler, VK_API_VERSION_1_0),

	// buffer.hpp
	VIL_DEV_HOOK(CreateBuffer, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroyBuffer, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(BindBufferMemory, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(GetBufferMemoryRequirements, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(GetBufferMemoryRequirements2, VK_API_VERSION_1_1),
	VIL_DEV_HOOK_ALIAS_CORE(GetBufferMemoryRequirements2KHR, GetBufferMemoryRequirements2,
		VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME),
	VIL_DEV_HOOK(BindBufferMemory2, VK_API_VERSION_1_1),
	VIL_DEV_HOOK_ALIAS_CORE(BindBufferMemory2KHR, BindBufferMemory2,
		VK_KHR_BIND_MEMORY_2_EXTENSION_NAME),

	VIL_DEV_HOOK(GetBufferDeviceAddress, VK_API_VERSION_1_2),
	VIL_DEV_HOOK(GetBufferOpaqueCaptureAddress, VK_API_VERSION_1_2),
	VIL_DEV_HOOK(GetDeviceMemoryOpaqueCaptureAddress, VK_API_VERSION_1_2),
	VIL_DEV_HOOK_ALIAS_CORE(GetBufferDeviceAddressKHR, GetBufferDeviceAddress,
		VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(GetBufferOpaqueCaptureAddressKHR, GetBufferOpaqueCaptureAddress,
		VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(GetDeviceMemoryOpaqueCaptureAddressKHR, GetDeviceMemoryOpaqueCaptureAddress,
		VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(GetBufferDeviceAddressEXT, GetBufferDeviceAddress,
		VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME),

	VIL_DEV_HOOK(CreateBufferView, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroyBufferView, VK_API_VERSION_1_0),

	// memory.hpp
	VIL_DEV_HOOK(AllocateMemory, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(FreeMemory, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(MapMemory, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(UnmapMemory, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(FlushMappedMemoryRanges, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(InvalidateMappedMemoryRanges, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(GetDeviceMemoryCommitment, VK_API_VERSION_1_0),

	// shader.hpp
	VIL_DEV_HOOK(CreateShaderModule, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroyShaderModule, VK_API_VERSION_1_0),

	// sync.hpp
	VIL_DEV_HOOK(CreateFence, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroyFence, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(ResetFences, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(GetFenceStatus, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(WaitForFences, VK_API_VERSION_1_0),

	VIL_DEV_HOOK(CreateSemaphore, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroySemaphore, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(SignalSemaphore, VK_API_VERSION_1_2),
	VIL_DEV_HOOK(WaitSemaphores, VK_API_VERSION_1_2),
	VIL_DEV_HOOK_ALIAS_CORE(SignalSemaphoreKHR, SignalSemaphore,
		VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(WaitSemaphoresKHR, WaitSemaphores,
		VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME),

	VIL_DEV_HOOK(CreateEvent, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroyEvent, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(SetEvent, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(ResetEvent, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(GetEventStatus, VK_API_VERSION_1_0),

	// ds.hpp
	VIL_DEV_HOOK(CreateDescriptorSetLayout, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroyDescriptorSetLayout, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CreateDescriptorPool, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroyDescriptorPool, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(ResetDescriptorPool, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(AllocateDescriptorSets, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(FreeDescriptorSets, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(UpdateDescriptorSets, VK_API_VERSION_1_0),

	VIL_DEV_HOOK(CreateDescriptorUpdateTemplate, VK_API_VERSION_1_1),
	VIL_DEV_HOOK(DestroyDescriptorUpdateTemplate, VK_API_VERSION_1_1),
	VIL_DEV_HOOK(UpdateDescriptorSetWithTemplate, VK_API_VERSION_1_1),
	VIL_DEV_HOOK(GetDescriptorSetLayoutSupport, VK_API_VERSION_1_1),
	VIL_DEV_HOOK_ALIAS_CORE(CreateDescriptorUpdateTemplateKHR,
		CreateDescriptorUpdateTemplate, VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(DestroyDescriptorUpdateTemplateKHR,
		DestroyDescriptorUpdateTemplate, VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(UpdateDescriptorSetWithTemplateKHR,
		UpdateDescriptorSetWithTemplate, VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(GetDescriptorSetLayoutSupportKHR,
		GetDescriptorSetLayoutSupport, VK_KHR_MAINTENANCE_3_EXTENSION_NAME),

	// pipe.hpp
	VIL_DEV_HOOK(CreateGraphicsPipelines, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CreateComputePipelines, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroyPipeline, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CreatePipelineLayout, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroyPipelineLayout, VK_API_VERSION_1_0),

	// queryPool.hpp
	VIL_DEV_HOOK(CreateQueryPool, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroyQueryPool, VK_API_VERSION_1_0),

	// accelStruct.hpp
	VIL_DEV_HOOK_EXT(CreateAccelerationStructureKHR, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(DestroyAccelerationStructureKHR, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(BuildAccelerationStructuresKHR, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CopyAccelerationStructureKHR, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CopyAccelerationStructureToMemoryKHR, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CopyMemoryToAccelerationStructureKHR, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(WriteAccelerationStructuresPropertiesKHR, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(GetAccelerationStructureDeviceAddressKHR, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(GetDeviceAccelerationStructureCompatibilityKHR, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(GetAccelerationStructureBuildSizesKHR, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),

	VIL_DEV_HOOK_EXT(CreateRayTracingPipelinesKHR, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(GetRayTracingCaptureReplayShaderGroupHandlesKHR, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(GetRayTracingShaderGroupStackSizeKHR, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(GetRayTracingShaderGroupHandlesKHR, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME),

	// cb.hpp
	VIL_DEV_HOOK(CreateCommandPool, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(DestroyCommandPool, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(ResetCommandPool, VK_API_VERSION_1_0),

	VIL_DEV_HOOK(TrimCommandPool, VK_API_VERSION_1_1),
	VIL_DEV_HOOK_ALIAS(TrimCommandPoolKHR, TrimCommandPool,
		VK_KHR_MAINTENANCE1_EXTENSION_NAME),

	VIL_DEV_HOOK(AllocateCommandBuffers, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(FreeCommandBuffers, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(BeginCommandBuffer, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(EndCommandBuffer, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(ResetCommandBuffer, VK_API_VERSION_1_0),

	VIL_DEV_HOOK(CmdBeginRenderPass, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdEndRenderPass, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdNextSubpass, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdWaitEvents, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdPipelineBarrier, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdBeginQuery, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdEndQuery, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdResetQueryPool, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdWriteTimestamp, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdCopyQueryPoolResults, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdDraw, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdDrawIndexed, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdDrawIndirect, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdDrawIndexedIndirect, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdDispatch, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdDispatchIndirect, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdBindVertexBuffers, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdBindIndexBuffer, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdBindDescriptorSets, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdClearColorImage, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdClearDepthStencilImage, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdClearAttachments, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdResolveImage, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdSetEvent, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdResetEvent, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdCopyBufferToImage, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdCopyImageToBuffer, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdBlitImage, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdCopyImage, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdExecuteCommands, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdCopyBuffer, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdFillBuffer, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdUpdateBuffer, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdBindPipeline, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdPushConstants, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdSetViewport, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdSetScissor, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdSetLineWidth, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdSetDepthBias, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdSetBlendConstants, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdSetDepthBounds, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdSetStencilCompareMask, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdSetStencilWriteMask, VK_API_VERSION_1_0),
	VIL_DEV_HOOK(CmdSetStencilReference, VK_API_VERSION_1_0),

	VIL_DEV_HOOK(CmdDispatchBase, VK_API_VERSION_1_1),
	VIL_DEV_HOOK_ALIAS(CmdDispatchBaseKHR, CmdDispatchBase,
		VK_KHR_DEVICE_GROUP_EXTENSION_NAME),

	VIL_DEV_HOOK(CmdDrawIndirectCount, VK_API_VERSION_1_2),
	VIL_DEV_HOOK_ALIAS_CORE(CmdDrawIndirectCountKHR, CmdDrawIndirectCount,
		VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdDrawIndirectCountAMD, CmdDrawIndirectCount,
		VK_AMD_DRAW_INDIRECT_COUNT_EXTENSION_NAME),

	VIL_DEV_HOOK(CmdDrawIndexedIndirectCount, VK_API_VERSION_1_2),
	VIL_DEV_HOOK_ALIAS_CORE(CmdDrawIndexedIndirectCountKHR, CmdDrawIndexedIndirectCount,
		VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdDrawIndexedIndirectCountAMD, CmdDrawIndexedIndirectCount,
		VK_AMD_DRAW_INDIRECT_COUNT_EXTENSION_NAME),

	VIL_DEV_HOOK(CmdBeginRenderPass2, VK_API_VERSION_1_2),
	VIL_DEV_HOOK(CmdNextSubpass2, VK_API_VERSION_1_2),
	VIL_DEV_HOOK(CmdEndRenderPass2, VK_API_VERSION_1_2),
	VIL_DEV_HOOK_ALIAS_CORE(CmdBeginRenderPass2KHR, CmdBeginRenderPass2,
		VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdNextSubpass2KHR, CmdNextSubpass2,
		VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdEndRenderPass2KHR, CmdEndRenderPass2,
		VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME),

	VIL_DEV_HOOK_EXT(CmdBeginDebugUtilsLabelEXT, VK_EXT_DEBUG_UTILS_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdEndDebugUtilsLabelEXT, VK_EXT_DEBUG_UTILS_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdInsertDebugUtilsLabelEXT, VK_EXT_DEBUG_UTILS_EXTENSION_NAME),

	// copy_commands2
	VIL_DEV_HOOK(CmdCopyBuffer2, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdCopyImage2, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdCopyBufferToImage2, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdCopyImageToBuffer2, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdBlitImage2, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdResolveImage2, VK_API_VERSION_1_3),
	VIL_DEV_HOOK_ALIAS_CORE(CmdCopyBuffer2KHR, CmdCopyBuffer2,
		VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdCopyImage2KHR, CmdCopyImage2,
		VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdCopyBufferToImage2KHR, CmdCopyBufferToImage2,
		VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdCopyImageToBuffer2KHR, CmdCopyImageToBuffer2,
		VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdBlitImage2KHR, CmdBlitImage2,
		VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdResolveImage2KHR, CmdResolveImage2,
		VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME),

	VIL_DEV_HOOK_EXT(CmdPushDescriptorSetKHR, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdPushDescriptorSetWithTemplateKHR, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME),

	VIL_DEV_HOOK_EXT(CmdSetFragmentShadingRateKHR, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME),

	VIL_DEV_HOOK_EXT(CmdBeginConditionalRenderingEXT, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdEndConditionalRenderingEXT, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME),

	VIL_DEV_HOOK_EXT(CmdSetLineStippleEXT, VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME),

	// extended dynamic state
	VIL_DEV_HOOK(CmdSetCullMode, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdSetFrontFace, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdSetPrimitiveTopology, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdSetViewportWithCount, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdSetScissorWithCount, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdBindVertexBuffers2, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdSetDepthTestEnable, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdSetDepthWriteEnable, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdSetDepthCompareOp, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdSetDepthBoundsTestEnable, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdSetStencilTestEnable, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdSetStencilOp, VK_API_VERSION_1_3),
	VIL_DEV_HOOK_ALIAS_CORE(CmdSetCullModeEXT, CmdSetCullMode,
		VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdSetFrontFaceEXT, CmdSetFrontFace,
		VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdSetPrimitiveTopologyEXT, CmdSetPrimitiveTopology,
		VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdSetViewportWithCountEXT, CmdSetViewportWithCount,
		VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdSetScissorWithCountEXT, CmdSetScissorWithCount,
		VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdBindVertexBuffers2EXT, CmdBindVertexBuffers2,
		VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdSetDepthTestEnableEXT, CmdSetDepthTestEnable,
		VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdSetDepthWriteEnableEXT, CmdSetDepthWriteEnable,
		VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdSetDepthCompareOpEXT, CmdSetDepthCompareOp,
		VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdSetDepthBoundsTestEnableEXT, CmdSetDepthBoundsTestEnable,
		VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdSetStencilTestEnableEXT, CmdSetStencilTestEnable,
		VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdSetStencilOpEXT, CmdSetStencilOp,
		VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME),

	VIL_DEV_HOOK_EXT(CmdSetPatchControlPointsEXT, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdSetRasterizerDiscardEnableEXT, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdSetDepthBiasEnableEXT, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdSetLogicOpEXT, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdSetPrimitiveRestartEnableEXT, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME),

	VIL_DEV_HOOK(CmdBeginRendering, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdEndRendering, VK_API_VERSION_1_3),
	VIL_DEV_HOOK_ALIAS_CORE(CmdBeginRenderingKHR, CmdBeginRendering,
		VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdEndRenderingKHR, CmdEndRendering,
		VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME),

	VIL_DEV_HOOK_EXT(CmdSetSampleLocationsEXT, VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME),

	VIL_DEV_HOOK_EXT(CmdSetDiscardRectangleEXT, VK_EXT_DISCARD_RECTANGLES_EXTENSION_NAME),

	VIL_DEV_HOOK_EXT(CmdBuildAccelerationStructuresKHR, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdBuildAccelerationStructuresIndirectKHR, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdCopyAccelerationStructureKHR, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdCopyAccelerationStructureToMemoryKHR, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdCopyMemoryToAccelerationStructureKHR, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdWriteAccelerationStructuresPropertiesKHR, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),

	VIL_DEV_HOOK_EXT(CmdTraceRaysKHR, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdTraceRaysIndirectKHR, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdSetRayTracingPipelineStackSizeKHR, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME),

	// sync2
	VIL_DEV_HOOK(CmdSetEvent2, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdResetEvent2, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdWaitEvents2, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdPipelineBarrier2, VK_API_VERSION_1_3),
	VIL_DEV_HOOK(CmdWriteTimestamp2, VK_API_VERSION_1_3),
	VIL_DEV_HOOK_ALIAS_CORE(CmdSetEvent2KHR, CmdSetEvent2,
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdResetEvent2KHR, CmdResetEvent2,
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdWaitEvents2KHR, CmdWaitEvents2,
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdPipelineBarrier2KHR, CmdPipelineBarrier2,
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS_CORE(CmdWriteTimestamp2KHR, CmdWriteTimestamp2,
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME),

	VIL_DEV_HOOK_EXT(CmdSetVertexInputEXT, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME),

	VIL_DEV_HOOK_EXT(CmdSetColorWriteEnableEXT, VK_EXT_COLOR_WRITE_ENABLE_EXTENSION_NAME),

	VIL_DEV_HOOK_EXT(CmdDrawMultiEXT, VK_EXT_MULTI_DRAW_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdDrawMultiIndexedEXT, VK_EXT_MULTI_DRAW_EXTENSION_NAME),

	// For testing.
	// VIL_DEV_HOOK_EXT(GetImageViewAddressNVX, VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME),
	// VIL_DEV_HOOK_EXT(GetImageViewHandleNVX, VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME),
	// VIL_DEV_HOOK_EXT(CmdCuLaunchKernelNVX, VK_NVX_BINARY_IMPORT_EXTENSION_NAME),
};

#undef VIL_INI_HOOK
#undef VIL_INI_HOOK_EXT

#undef VIL_DEV_HOOK
#undef VIL_DEV_HOOK_EXT
#undef VIL_DEV_HOOK_ALIAS

// We make sure this way that e.g. calling vkGetInstanceProcAddr with
// vkGetInstanceProcAddr as funcName parameter returns itself.
// Not sure how important that is though.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance ini, const char* funcName) {
	// Check if we hooked it. If we didn't hook it and ini is invalid,
	// return nullptr.
	auto it = funcPtrTable.find(std::string_view(funcName));
	if(it == funcPtrTable.end()) {
		// If it's not hooked, just forward it to the next chain link
		auto* inid = vil::findData<vil::Instance>(ini);
		if(!inid || !inid->dispatch.GetInstanceProcAddr) {
			dlg_error("invalid instance data: {}", ini);
			return nullptr;
		}

		return inid->dispatch.GetInstanceProcAddr(ini, funcName);
	}

	// special case: functions that don't need instance.
	auto& hook = it->second;
	if(std::strcmp(funcName, "vkGetInstanceProcAddr") == 0 ||
			// NOTE: seems that some applications need this even though
			// it shouldn't be valid use per spec
			std::strcmp(funcName, "vkCreateDevice") == 0 ||
			std::strcmp(funcName, "vkCreateInstance") == 0) {
		return hook.func;
	}

	if(!ini) {
		dlg_trace("Invalid call to GetInstanceProcAddr without ini: {}", funcName);
		return nullptr;
	}

	auto* inid = vil::findData<vil::Instance>(ini);
	if(!inid) {
		dlg_error("invalid instance data: {}", ini);
		return nullptr;
	}

	if(!hook.device && !hook.iniExt.empty()) {
		auto it = find(inid->extensions, hook.iniExt);
		if(it == inid->extensions.end()) {
			return inid->dispatch.GetInstanceProcAddr(ini, funcName);
			// return nullptr;
		}
	}

	// TODO: consider instance version

	// TODO: when the queried function is a device function we technically
	// should only return it when there is a physical device supporting
	// the function (we could store a list of those in the instance data).
	// See documentation for vkGetInstanceProcAddr.

	return hook.func;
}

auto knownUnhooked = std::set<std::string>{
	"vkGetDeviceQueue",
	"vkGetDeviceQueue2",
	// query pool
	"vkGetQueryPoolResults",
	"vkGetDescriptorSetLayoutSupport",
	// pipeline cache
	"vkCreatePipelineCache",
	"vkMergePipelineCaches",
	"vkDestroyPipelineCache",
	"vkGetPipelineCacheData",
	// general query
	"vkGetRenderAreaGranularity",
	"vkGetDeviceGroupPeerMemoryFeatures",
	"vkGetImageSparseMemoryRequirements2",
	"vkGetDeviceBufferMemoryRequirements",
	"vkGetDeviceImageMemoryRequirements",
	"vkGetDeviceImageSparseMemoryRequirements",
	"vkGetDeviceGroupPresentCapabilitiesKHR",
	"vkGetDeviceGroupSurfacePresentModesKHR",
	"vkGetDeviceGroupPeerMemoryFeaturesKHR",
	"vkGetImageSparseMemoryRequirements2KHR",
	// private data stuff
	"vkCreatePrivateDataSlot",
	"vkDestroyPrivateDataSlot",
	"vkSetPrivateData",
	"vkGetPrivateData",

	// the ones we need to hook
	// vkCmdSetDeviceMask
	// vkCmdSetRasterizerDiscardEnable
	// vkCmdSetDepthBiasEnable
	// vkCmdSetPrimitiveRestartEnable
	//
	// vkCreateSamplerYcbcrConversion
	// vkDestroySamplerYcbcrConversion
	// vkGetDescriptorSetLayoutSupport // for samplers!
	// vkCreateSamplerYcbcrConversionKHR
	// vkDestroySamplerYcbcrConversionKHR
	// vkGetDescriptorSetLayoutSupportKHR
	//
	// vkGetSemaphoreCounterValue // for semaphore wrapping at some point?
	//
	// vkCreateSharedSwapchainsKHR
	// vkCreateVideoSessionKHR
	// vkDestroyVideoSessionKHR
	// vkGetVideoSessionMemoryRequirementsKHR
	// vkBindVideoSessionMemoryKHR
	// vkCreateVideoSessionParametersKHR
	// vkUpdateVideoSessionParametersKHR
	// vkDestroyVideoSessionParametersKHR
	// vkCmdBeginVideoCodingKHR
	// vkCmdEndVideoCodingKHR
	// vkCmdControlVideoCodingKHR
	// vkCmdDecodeVideoKHR
	//
	// vkCmdSetDeviceMaskKHR
	//
	// vkCmdEncodeVideoKHR
	// vkCmdWriteBufferMarker2AMD
	// vkGetQueueCheckpointData2NV
	// vkCmdTraceRaysIndirect2KHR

	// resource import/export
	// vkGetMemoryFdKHR
	// vkGetMemoryFdPropertiesKHR
	// vkImportSemaphoreFdKHR
	// vkGetSemaphoreFdKHR
	// vkGetSwapchainStatusKHR
	// vkImportFenceFdKHR
	// vkGetFenceFdKHR
	// vkAcquireProfilingLockKHR
	// vkReleaseProfilingLockKHR
};

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice vkDev, const char* funcName) {
	auto it = funcPtrTable.find(std::string_view(funcName));
	if(it == funcPtrTable.end()) {
		// If it's not hooked, just forward it to the next chain link
		if(knownUnhooked.find(funcName) == knownUnhooked.end()) {
			dlg_trace("unhooked device function: {}", funcName);
		}

		auto* dev = vil::findData<vil::Device>(vkDev);
		if(!dev || !dev->dispatch.GetDeviceProcAddr) {
			dlg_error("invalid device data: {}", vkDev);
			return nullptr;
		}

  		return dev->dispatch.GetDeviceProcAddr(vkDev, funcName);
	}

	auto& hook = it->second;
	if(!vkDev || !hook.device) {
		dlg_trace("GetDeviceProcAddr with no devcice/non-device function: {}", funcName);
		return nullptr;
	}

	auto* dev = vil::findData<vil::Device>(vkDev);
	if(!dev) {
		dlg_error("invalid device data: {}", vkDev);
		return nullptr;
	}

	if(!hook.devExt.empty()) {
		auto it = find(dev->appExts, hook.devExt);
		if(it == dev->appExts.end()) {
			return nullptr;
			// return dev->dispatch.GetDeviceProcAddr(vkDev, funcName);
		}
	}

	// TODO: consider device version?
	if(dev->ini->app.apiVersion < hook.version) {
		return nullptr;
	}

	return hook.func;
}

} // namespace vil

// Global layer entry points
extern "C" VIL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance ini, const char* funcName) {
	return vil::GetInstanceProcAddr(ini, funcName);
}

extern "C" VIL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice dev, const char* funcName) {
	return vil::GetDeviceProcAddr(dev, funcName);
}

/*
extern "C" VIL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vil_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
	// dlg_trace("vkNegotiateLoaderLayerInterfaceVersion");

	pVersionStruct->pfnGetDeviceProcAddr = &vkGetDeviceProcAddr;
	pVersionStruct->pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
	// This whole function was introduced in version 2, so it can't
	// be below that.
	dlg_assert(pVersionStruct->loaderLayerInterfaceVersion >= 2);
	pVersionStruct->loaderLayerInterfaceVersion = 2;

	return VK_SUCCESS;
}
*/

// Util for integration testing
// TODO: only enable if integration tests are enabled
extern "C" VIL_EXPORT int vil_getErrorWarningCount() {
	return vil::dlgWarnErrorCount;
}
