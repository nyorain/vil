#include <layer.hpp>
#include <data.hpp>
#include <swapchain.hpp>
#include <handles.hpp>
#include <platform.hpp>
#include <queue.hpp>
#include <overlay.hpp>
#include <commands.hpp>
#include <util/util.hpp>

#ifdef FUEN_WITH_WAYLAND
  #include <wayland.hpp>
#endif // FUEN_WITH_WAYLAND

#ifdef FUEN_WITH_X11
  #include <xlib.hpp>
#endif // FUEN_WITH_X11

#ifdef FUEN_WITH_WIN32
  #include <win32.hpp>
#endif // FUEN_WITH_WIN32

#include <vk/dispatch_table_helper.h>
#include <dlg/dlg.hpp>
#include <csignal>

#include <vil_api.h>

namespace vil {

// Util
// #define BREAK_ON_ERROR
#ifdef BREAK_ON_ERROR
void dlgHandler(const struct dlg_origin* origin, const char* string, void* data) {
	if (origin->level >= dlg_level_error) {
		// break
		// TODO: should be disabled in non-debug modes (but all of dlg probably should be?)
		// std::raise(SIGABRT);
		// DebugBreak();
	}
	dlg_default_output(origin, string, data);
	// (void) string;
	// (void) data;
}
#endif // BREAK_ON_ERROR

std::size_t DescriptorSetRef::Hash::operator()(const DescriptorSetRef& dsr) const noexcept {
	auto h = std::size_t(0);
	hash_combine(h, dsr.ds);
	hash_combine(h, dsr.binding);
	hash_combine(h, dsr.elem);
	return h;
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

// Instance
VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(
		const VkInstanceCreateInfo* ci,
		const VkAllocationCallbacks* alloc,
		VkInstance* pInstance) {

#ifdef BREAK_ON_ERROR
	// Even in debug kinda unacceptable.
	// Maybe acceptable if we just reuse the old handler (and restore on DestroyInstance). Might still have problems
	dlg_set_handler(dlgHandler, nullptr);
#endif // BREAK_ON_ERROR

	// TODO: remove/find real solution
#ifdef _WIN32
	AllocConsole();
	dlg_trace("Allocated console. Creating vulkan instance");
#endif // _WIN32

	dlg_trace("CreateInstance");

	auto* linkInfo = findChainInfo<VkLayerInstanceCreateInfo, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO>(*ci);
	while(linkInfo && linkInfo->function != VK_LAYER_LINK_INFO) {
		linkInfo = findChainInfo<VkLayerInstanceCreateInfo, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO>(*linkInfo);
	}

	if(!linkInfo) {
		dlg_error("No linkInfo");
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	auto fpGetInstanceProcAddr = linkInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	auto fpCreateInstance = (PFN_vkCreateInstance)fpGetInstanceProcAddr(nullptr, "vkCreateInstance");
	if(!fpCreateInstance) {
		dlg_error("could not load vkCreateInstance");
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	// Advance the link info for the next element on the chain
	auto mutLinkInfo = const_cast<VkLayerInstanceCreateInfo*>(linkInfo);
	mutLinkInfo->u.pLayerInfo = linkInfo->u.pLayerInfo->pNext;

	// Init instance data
	// NOTE: we cannot add extensions here, sadly.
	// See https://github.com/KhronosGroup/Vulkan-Loader/issues/51
	auto nci = *ci;

	// TODO: we can't call vkEnumerateInstanceVersion ourselves.
	// So we just trial-and-error to possibly bump up the version.
	// When instance creation fails just turn it down to original again.
	auto originalApiVersion = VK_API_VERSION_1_0;
	auto ourApiVersion = VK_API_VERSION_1_2;

	VkApplicationInfo ourAppInfo {};
	if(!nci.pApplicationInfo) {
		dlg_debug("Trying to manually increase instance apiVersion");
		ourAppInfo.apiVersion = ourApiVersion;
		nci.pApplicationInfo = &ourAppInfo;
	} else {
		originalApiVersion = nci.pApplicationInfo->apiVersion;
		if(nci.pApplicationInfo->apiVersion < ourApiVersion) {
			dlg_debug("Trying to manually increase instance apiVersion");
			ourAppInfo = *nci.pApplicationInfo;
			ourAppInfo.apiVersion = ourApiVersion;
			nci.pApplicationInfo = &ourAppInfo;
		}
	}

	// == Create instance ==
	VkResult result = fpCreateInstance(&nci, alloc, pInstance);
	if(result != VK_SUCCESS) {
		if(ourApiVersion <= originalApiVersion) {
			return result;
		}

		dlg_debug("Bumping up instance API version failed, trying without");
		dlg_assert(nci.pApplicationInfo == &ourAppInfo);
		ourApiVersion = originalApiVersion;
		ourAppInfo.apiVersion = originalApiVersion;
		result = fpCreateInstance(&nci, alloc, pInstance);

		if(result != VK_SUCCESS) {
			return result;
		}
	}

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
		ini.app.apiVersion = ci->pApplicationInfo->apiVersion;
		ini.app.version = ci->pApplicationInfo->applicationVersion;
		ini.app.name = strOrEmpty(ci->pApplicationInfo->pApplicationName);
		ini.app.engineName = strOrEmpty(ci->pApplicationInfo->pEngineName);
		ini.app.engineVersion = ci->pApplicationInfo->engineVersion;
	}

	ini.vulkan11 = (ourApiVersion >= VK_API_VERSION_1_1);
	ini.vulkan12 = (ourApiVersion >= VK_API_VERSION_1_2);

	layer_init_instance_dispatch_table(*pInstance, &ini.dispatch, fpGetInstanceProcAddr);

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
	auto phdevs = std::make_unique<VkPhysicalDevice[]>(phdevCount);
	ini.dispatch.EnumeratePhysicalDevices(*pInstance, &phdevCount, phdevs.get());

	for(auto i = 0u; i < phdevCount; ++i) {
		insertData(phdevs[i], &ini);
	}

	return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance ini, const VkAllocationCallbacks* alloc) {
	if(!ini) {
		return;
	}

	auto inid = moveData<Instance>(ini);
	dlg_assert(inid);
	inid->dispatch.DestroyInstance(ini, alloc);
}

VKAPI_ATTR PFN_vkVoidFunction GetInstanceProcAddr(VkInstance, const char*);
VKAPI_ATTR PFN_vkVoidFunction GetDeviceProcAddr(VkDevice, const char*);

struct HookedFunction {
	PFN_vkVoidFunction func {};
	bool device {}; // device-level function
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

#define VIL_INI_HOOK(fn) {"vk" # fn, {(PFN_vkVoidFunction) fn, FN_TC(fn, false), {}}}
#define VIL_INI_HOOK_EXT(fn, ext) {"vk" # fn, {(PFN_vkVoidFunction) fn, FN_TC(fn, false), ext}}

#define VIL_DEV_HOOK(fn) {"vk" # fn, {(PFN_vkVoidFunction) fn, FN_TC(fn, true), {}, {}}}
#define VIL_DEV_HOOK_EXT(fn, ext) {"vk" # fn, {(PFN_vkVoidFunction) fn, FN_TC(fn, true), {}, ext}}
#define VIL_DEV_HOOK_ALIAS(alias, fn, ext) {"vk" # alias, {(PFN_vkVoidFunction) fn, FN_TC_ALIAS(alias, fn, true), {}, ext}}

static const std::unordered_map<std::string_view, HookedFunction> funcPtrTable {
	VIL_INI_HOOK(GetInstanceProcAddr),
	VIL_INI_HOOK(CreateInstance),
	VIL_INI_HOOK(DestroyInstance),

	VIL_DEV_HOOK(GetDeviceProcAddr),
	VIL_DEV_HOOK(CreateDevice),
	VIL_DEV_HOOK(DestroyDevice),
	VIL_DEV_HOOK(DeviceWaitIdle),

	VIL_DEV_HOOK(QueueSubmit),
	VIL_DEV_HOOK(QueueWaitIdle),

	VIL_DEV_HOOK_EXT(CreateSwapchainKHR, VK_KHR_SWAPCHAIN_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(DestroySwapchainKHR, VK_KHR_SWAPCHAIN_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(QueuePresentKHR, VK_KHR_SWAPCHAIN_EXTENSION_NAME),

	VIL_DEV_HOOK_EXT(SetDebugUtilsObjectNameEXT, VK_EXT_DEBUG_UTILS_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(SetDebugUtilsObjectTagEXT, VK_EXT_DEBUG_UTILS_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdBeginDebugUtilsLabelEXT, VK_EXT_DEBUG_UTILS_EXTENSION_NAME),
	VIL_DEV_HOOK_EXT(CmdEndDebugUtilsLabelEXT, VK_EXT_DEBUG_UTILS_EXTENSION_NAME),

#ifdef VIL_WITH_WAYLAND
	VIL_HOOK(CreateWaylandSurfaceKHR),
#endif // VIL_WITH_WAYLAND

#ifdef VIL_WITH_X11
	VIL_INI_HOOK_EXT(CreateXlibSurfaceKHR, VK_KHR_XLIB_SURFACE_EXTENSION_NAME),
	VIL_INI_HOOK_EXT(CreateXcbSurfaceKHR, VK_KHR_XCB_SURFACE_EXTENSION_NAME),
#endif // VIL_WITH_X11

#ifdef VIL_WITH_WIN32
	// We do everything to not include the platform-specific header
	// VIL_INI_HOOK_EXT(CreateWin32SurfaceKHR, VK_KHR_WIN32_SURFACE_EXTENSION_NAME),
	{"vkCreateWin32SurfaceKHR", HookedFunction{(PFN_vkVoidFunction) CreateWin32SurfaceKHR, false, "VK_KHR_win32_surface"}},
#endif // VIL_WITH_WIN32

	VIL_INI_HOOK_EXT(DestroySurfaceKHR, VK_KHR_SURFACE_EXTENSION_NAME),

	// rp.hpp
	VIL_DEV_HOOK(CreateFramebuffer),
	VIL_DEV_HOOK(DestroyFramebuffer),

	VIL_DEV_HOOK(CreateRenderPass),
	VIL_DEV_HOOK(DestroyRenderPass),
	VIL_DEV_HOOK(CreateRenderPass2),
	VIL_DEV_HOOK_ALIAS(CreateRenderPass2KHR, CreateRenderPass2,
		VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME),

	// image.hpp
	VIL_DEV_HOOK(CreateImage),
	VIL_DEV_HOOK(DestroyImage),
	VIL_DEV_HOOK(BindImageMemory),
	VIL_DEV_HOOK(BindImageMemory2),
	VIL_DEV_HOOK_ALIAS(BindImageMemory2KHR, BindImageMemory2,
		VK_KHR_BIND_MEMORY_2_EXTENSION_NAME),

	VIL_DEV_HOOK(CreateImageView),
	VIL_DEV_HOOK(DestroyImageView),

	VIL_DEV_HOOK(CreateSampler),
	VIL_DEV_HOOK(DestroySampler),

	// buffer.hpp
	VIL_DEV_HOOK(CreateBuffer),
	VIL_DEV_HOOK(DestroyBuffer),
	VIL_DEV_HOOK(BindBufferMemory),
	VIL_DEV_HOOK(BindBufferMemory2),
	VIL_DEV_HOOK_ALIAS(BindBufferMemory2KHR, BindBufferMemory2,
		VK_KHR_BIND_MEMORY_2_EXTENSION_NAME),

	VIL_DEV_HOOK(CreateBufferView),
	VIL_DEV_HOOK(DestroyBufferView),

	// memory.hpp
	VIL_DEV_HOOK(AllocateMemory),
	VIL_DEV_HOOK(FreeMemory),
	VIL_DEV_HOOK(MapMemory),
	VIL_DEV_HOOK(UnmapMemory),

	// shader.hpp
	VIL_DEV_HOOK(CreateShaderModule),
	VIL_DEV_HOOK(DestroyShaderModule),

	// sync.hpp
	VIL_DEV_HOOK(CreateFence),
	VIL_DEV_HOOK(DestroyFence),
	VIL_DEV_HOOK(ResetFences),
	VIL_DEV_HOOK(GetFenceStatus),
	VIL_DEV_HOOK(WaitForFences),

	VIL_DEV_HOOK(CreateSemaphore),
	VIL_DEV_HOOK(DestroySemaphore),

	VIL_DEV_HOOK(CreateEvent),
	VIL_DEV_HOOK(DestroyEvent),
	VIL_DEV_HOOK(SetEvent),
	VIL_DEV_HOOK(ResetEvent),
	VIL_DEV_HOOK(GetEventStatus),

	// ds.hpp
	VIL_DEV_HOOK(CreateDescriptorSetLayout),
	VIL_DEV_HOOK(DestroyDescriptorSetLayout),
	VIL_DEV_HOOK(CreateDescriptorPool),
	VIL_DEV_HOOK(DestroyDescriptorPool),
	VIL_DEV_HOOK(ResetDescriptorPool),
	VIL_DEV_HOOK(AllocateDescriptorSets),
	VIL_DEV_HOOK(FreeDescriptorSets),
	VIL_DEV_HOOK(UpdateDescriptorSets),

	VIL_DEV_HOOK(CreateDescriptorUpdateTemplate),
	VIL_DEV_HOOK(DestroyDescriptorUpdateTemplate),
	VIL_DEV_HOOK(UpdateDescriptorSetWithTemplate),
	VIL_DEV_HOOK_ALIAS(CreateDescriptorUpdateTemplateKHR,
		CreateDescriptorUpdateTemplate, VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS(DestroyDescriptorUpdateTemplateKHR,
		DestroyDescriptorUpdateTemplate, VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS(UpdateDescriptorSetWithTemplateKHR,
		UpdateDescriptorSetWithTemplate, VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME),

	// pipe.hpp
	VIL_DEV_HOOK(CreateGraphicsPipelines),
	VIL_DEV_HOOK(CreateComputePipelines),
	VIL_DEV_HOOK(DestroyPipeline),
	VIL_DEV_HOOK(CreatePipelineLayout),
	VIL_DEV_HOOK(DestroyPipelineLayout),

	// queryPool.hpp
	VIL_DEV_HOOK(CreateQueryPool),
	VIL_DEV_HOOK(DestroyQueryPool),

	// cb.hpp
	VIL_DEV_HOOK(CreateCommandPool),
	VIL_DEV_HOOK(DestroyCommandPool),
	VIL_DEV_HOOK(ResetCommandPool),

	VIL_DEV_HOOK(TrimCommandPool),
	VIL_DEV_HOOK_ALIAS(TrimCommandPoolKHR, TrimCommandPool,
		VK_KHR_MAINTENANCE1_EXTENSION_NAME),

	VIL_DEV_HOOK(AllocateCommandBuffers),
	VIL_DEV_HOOK(FreeCommandBuffers),
	VIL_DEV_HOOK(BeginCommandBuffer),
	VIL_DEV_HOOK(EndCommandBuffer),
	VIL_DEV_HOOK(ResetCommandBuffer),

	VIL_DEV_HOOK(CmdBeginRenderPass),
	VIL_DEV_HOOK(CmdEndRenderPass),
	VIL_DEV_HOOK(CmdNextSubpass),
	VIL_DEV_HOOK(CmdWaitEvents),
	VIL_DEV_HOOK(CmdPipelineBarrier),
	VIL_DEV_HOOK(CmdBeginQuery),
	VIL_DEV_HOOK(CmdEndQuery),
	VIL_DEV_HOOK(CmdResetQueryPool),
	VIL_DEV_HOOK(CmdWriteTimestamp),
	VIL_DEV_HOOK(CmdCopyQueryPoolResults),
	VIL_DEV_HOOK(CmdDraw),
	VIL_DEV_HOOK(CmdDrawIndexed),
	VIL_DEV_HOOK(CmdDrawIndirect),
	VIL_DEV_HOOK(CmdDrawIndexedIndirect),
	VIL_DEV_HOOK(CmdDispatch),
	VIL_DEV_HOOK(CmdDispatchIndirect),
	VIL_DEV_HOOK(CmdBindVertexBuffers),
	VIL_DEV_HOOK(CmdBindIndexBuffer),
	VIL_DEV_HOOK(CmdBindDescriptorSets),
	VIL_DEV_HOOK(CmdClearColorImage),
	VIL_DEV_HOOK(CmdClearDepthStencilImage),
	VIL_DEV_HOOK(CmdClearAttachments),
	VIL_DEV_HOOK(CmdResolveImage),
	VIL_DEV_HOOK(CmdSetEvent),
	VIL_DEV_HOOK(CmdResetEvent),
	VIL_DEV_HOOK(CmdCopyBufferToImage),
	VIL_DEV_HOOK(CmdCopyImageToBuffer),
	VIL_DEV_HOOK(CmdBlitImage),
	VIL_DEV_HOOK(CmdCopyImage),
	VIL_DEV_HOOK(CmdExecuteCommands),
	VIL_DEV_HOOK(CmdCopyBuffer),
	VIL_DEV_HOOK(CmdFillBuffer),
	VIL_DEV_HOOK(CmdUpdateBuffer),
	VIL_DEV_HOOK(CmdBindPipeline),
	VIL_DEV_HOOK(CmdPushConstants),
	VIL_DEV_HOOK(CmdSetViewport),
	VIL_DEV_HOOK(CmdSetScissor),
	VIL_DEV_HOOK(CmdSetLineWidth),
	VIL_DEV_HOOK(CmdSetDepthBias),
	VIL_DEV_HOOK(CmdSetBlendConstants),
	VIL_DEV_HOOK(CmdSetDepthBounds),
	VIL_DEV_HOOK(CmdSetStencilCompareMask),
	VIL_DEV_HOOK(CmdSetStencilWriteMask),
	VIL_DEV_HOOK(CmdSetStencilReference),

	VIL_DEV_HOOK(CmdDispatchBase),
	VIL_DEV_HOOK_ALIAS(CmdDispatchBaseKHR, CmdDispatchBase,
		VK_KHR_DEVICE_GROUP_EXTENSION_NAME),

	VIL_DEV_HOOK(CmdDrawIndirectCount),
	VIL_DEV_HOOK_ALIAS(CmdDrawIndirectCountKHR, CmdDrawIndirectCount,
		VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS(CmdDrawIndirectCountAMD, CmdDrawIndirectCount,
		VK_AMD_DRAW_INDIRECT_COUNT_EXTENSION_NAME),

	VIL_DEV_HOOK(CmdDrawIndexedIndirectCount),
	VIL_DEV_HOOK_ALIAS(CmdDrawIndexedIndirectCountKHR, CmdDrawIndexedIndirectCount,
		VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME),
	VIL_DEV_HOOK_ALIAS(CmdDrawIndexedIndirectCountAMD, CmdDrawIndexedIndirectCount,
		VK_AMD_DRAW_INDIRECT_COUNT_EXTENSION_NAME),

	VIL_DEV_HOOK(CmdBeginRenderPass2),
	VIL_DEV_HOOK_ALIAS(CmdBeginRenderPass2KHR, CmdBeginRenderPass2,
		VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME),

	VIL_DEV_HOOK(CmdNextSubpass2),
	VIL_DEV_HOOK_ALIAS(CmdNextSubpass2KHR, CmdNextSubpass2,
		VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME),

	VIL_DEV_HOOK(CmdEndRenderPass2),
	VIL_DEV_HOOK_ALIAS(CmdEndRenderPass2KHR, CmdEndRenderPass2,
		VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME),
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
	auto& hooked = it->second;
	if(std::strcmp(funcName, "vkGetInstanceProcAddr") == 0 ||
			// TODO: ??? why is this needed for dota??
			std::strcmp(funcName, "vkCreateDevice") == 0 ||
			std::strcmp(funcName, "vkCreateInstance") == 0) {
		return hooked.func;
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

	if(!hooked.device && !hooked.iniExt.empty()) {
		auto it = find(inid->extensions, hooked.iniExt);
		if(it == inid->extensions.end()) {
			// dlg_trace("tried to load ini proc addr {} for disabled ext {}",
			// 	funcName, hooked.iniExt);
			// TODO: not sure what is better
			return inid->dispatch.GetInstanceProcAddr(ini, funcName);
			// return nullptr;
		}
	}

	// TODO: when the queried function is a device function we technically
	// should only return it when there is a physical device supporting
	// the function (we could store a list of those in the instance data).
	// See documentation for vkGetInstanceProcAddr.

	return hooked.func;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice vkDev, const char* funcName) {
	//(void) vkDev;
	//(void) funcName;

	//return reinterpret_cast<PFN_vkVoidFunction>(dummyDev);

	auto it = funcPtrTable.find(std::string_view(funcName));
	if(it == funcPtrTable.end()) {
		// If it's not hooked, just forward it to the next chain link
		auto* dev = vil::findData<vil::Device>(vkDev);
		if(!dev || !dev->dispatch.GetDeviceProcAddr) {
			dlg_error("invalid device data: {}", vkDev);
			return nullptr;
		}

  		return dev->dispatch.GetDeviceProcAddr(vkDev, funcName);
	}

	auto& hooked = it->second;
	if(!vkDev || !hooked.device) {
		dlg_trace("GetDeviceProcAddr with no devcice/non-device function: {}", funcName);
		return nullptr;
	}

	auto* dev = vil::findData<vil::Device>(vkDev);
	if(!dev) {
		dlg_error("invalid device data: {}", vkDev);
		return nullptr;
	}

	if(!hooked.devExt.empty()) {
		auto it = find(dev->appExts, hooked.devExt);
		if(it == dev->appExts.end()) {
			// dlg_trace("tried to load dev proc addr {} for disabled ext {}",
			// 	funcName, hooked.devExt);
			// TODO: not sure what is better
			return dev->dispatch.GetDeviceProcAddr(vkDev, funcName);
			// return nullptr;
		}
	}

	return hooked.func;
}

} // namespace vil

// TODO: not sure why we need this on windows
#ifdef _WIN32
	#define VIL_EXPORT __declspec(dllexport)
#else
	#define VIL_EXPORT VK_LAYER_EXPORT
#endif

// Global layer entry points
extern "C" VIL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance ini, const char* funcName) {
	// dlg_trace("vil get ini proc addr");
	return vil::GetInstanceProcAddr(ini, funcName);
}

extern "C" VIL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice dev, const char* funcName) {
	// dlg_trace("vil get device proc addr");
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
