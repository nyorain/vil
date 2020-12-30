#include <layer.hpp>
#include <util.hpp>
#include <data.hpp>
#include <swapchain.hpp>
#include <handles.hpp>
#include <platform.hpp>
#include <queue.hpp>
#include <overlay.hpp>

#include <wayland.hpp>
#include <xlib.hpp>

#include <commands.hpp>
#include <vk/dispatch_table_helper.h>
#include <dlg/dlg.hpp>
#include <swa/swa.h>
#include <csignal>

#include <fuen_api.h>

namespace fuen {

// Util
#define BREAK_ON_ERROR
#ifdef BREAK_ON_ERROR
void dlgHandler(const struct dlg_origin* origin, const char* string, void* data) {
	if (origin->level >= dlg_level_error) {
		// break
		// TODO: should be disabled in non-debug modes (but all of dlg probably should be?)
		// std::raise(SIGABRT);
		// DebugBreak();
	}
	dlg_default_output(origin, string, data);
}
#endif // BREAK_ON_ERROR

std::size_t DescriptorSetRef::Hash::operator()(const DescriptorSetRef& dsr) const noexcept {
	auto h = std::size_t(0);
	hash_combine(h, dsr.ds);
	hash_combine(h, dsr.binding);
	hash_combine(h, dsr.elem);
	return h;
}

// Classes
Instance::~Instance() {
	//if(display) {
	//	swa_display_destroy(display);
	//}
}

// Instance
VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(
		const VkInstanceCreateInfo* ci,
		const VkAllocationCallbacks* alloc,
		VkInstance* pInstance) {

	// TODO: urgh
	new(&dataMutex) decltype(dataMutex) {};

#ifdef BREAK_ON_ERROR
	// Even in debug kinda unacceptable.
	// Maybe acceptable if we just reuse the old handler (and restore on DestroyInstance). Might still have problems
	dlg_set_handler(dlgHandler, nullptr);
#endif // BREAK_ON_ERROR

	// TODO: remove/find real solution
#ifdef _WIN32
	AllocConsole();
	// dlg_trace("Allocated console. Creating vulkan instance");
	// dlg_error("Testing error");
#endif // _WIN32

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
	// Add additionally requiresd extensions
	auto iniPtr = std::make_unique<Instance>();
	auto& ini = *iniPtr;

	auto extsBegin = ci->ppEnabledExtensionNames;
	auto extsEnd = ci->ppEnabledExtensionNames + ci->enabledExtensionCount;

	auto nci = *ci;

	// Create instance
	VkResult result = fpCreateInstance(&nci, alloc, pInstance);
	if(result != VK_SUCCESS) {
		return result;
	}

	insertData(*pInstance, iniPtr.release());
	ini.handle = *pInstance;
	ini.extensions = {extsBegin, extsEnd};

	// TODO: could enable it ourselves. Might be useful even if
	// application does not want it.
	// But we can't check whether it's available, see extension
	// checks above :(
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

	layer_init_instance_dispatch_table(*pInstance, &ini.dispatch, fpGetInstanceProcAddr);

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

VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(
		VkInstance                                  instance,
		VkSurfaceKHR                                surface,
		const VkAllocationCallbacks*                pAllocator) {
	if(!surface) {
		return;
	}

	auto platform = moveDataOpt<Platform>(surface); // destroy it
	auto& ini = getData<Instance>(instance);
	ini.dispatch.DestroySurfaceKHR(instance, surface, pAllocator);
}

VKAPI_ATTR PFN_vkVoidFunction GetInstanceProcAddr(VkInstance, const char*);
VKAPI_ATTR PFN_vkVoidFunction GetDeviceProcAddr(VkDevice, const char*);

struct HookedFunction {
	PFN_vkVoidFunction func {};
	bool device {}; // device-level function
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

#define FUEN_INI_HOOK(fn) {"vk" # fn, {(PFN_vkVoidFunction) fn, FN_TC(fn, false), {}}}
#define FUEN_INI_HOOK_EXT(fn, ext) {"vk" # fn, {(PFN_vkVoidFunction) fn, FN_TC(fn, false), ext}}

#define FUEN_DEV_HOOK(fn) {"vk" # fn, {(PFN_vkVoidFunction) fn, FN_TC(fn, true), {}, {}}}
#define FUEN_DEV_HOOK_EXT(fn, ext) {"vk" # fn, {(PFN_vkVoidFunction) fn, FN_TC(fn, true), {}, ext}}
#define FUEN_DEV_HOOK_ALIAS(alias, fn, ext) {"vk" # alias, {(PFN_vkVoidFunction) fn, FN_TC_ALIAS(alias, fn, true), {}, ext}}

static const std::unordered_map<std::string_view, HookedFunction> funcPtrTable {
	FUEN_INI_HOOK(GetInstanceProcAddr),
	FUEN_INI_HOOK(CreateInstance),
	FUEN_INI_HOOK(DestroyInstance),

	FUEN_DEV_HOOK(GetDeviceProcAddr),
	FUEN_DEV_HOOK(CreateDevice),
	FUEN_DEV_HOOK(DestroyDevice),
	FUEN_DEV_HOOK(DeviceWaitIdle),

	FUEN_DEV_HOOK(QueueSubmit),
	FUEN_DEV_HOOK(QueueWaitIdle),

	FUEN_DEV_HOOK_EXT(CreateSwapchainKHR, VK_KHR_SWAPCHAIN_EXTENSION_NAME),
	FUEN_DEV_HOOK_EXT(DestroySwapchainKHR, VK_KHR_SWAPCHAIN_EXTENSION_NAME),
	FUEN_DEV_HOOK_EXT(QueuePresentKHR, VK_KHR_SWAPCHAIN_EXTENSION_NAME),

	FUEN_DEV_HOOK_EXT(SetDebugUtilsObjectNameEXT, VK_EXT_DEBUG_UTILS_EXTENSION_NAME),
	FUEN_DEV_HOOK_EXT(SetDebugUtilsObjectTagEXT, VK_EXT_DEBUG_UTILS_EXTENSION_NAME),
	FUEN_DEV_HOOK_EXT(CmdBeginDebugUtilsLabelEXT, VK_EXT_DEBUG_UTILS_EXTENSION_NAME),
	FUEN_DEV_HOOK_EXT(CmdEndDebugUtilsLabelEXT, VK_EXT_DEBUG_UTILS_EXTENSION_NAME),

	// TODO: enable optionally
	// FUEN_HOOK(CreateWaylandSurfaceKHR),
	FUEN_INI_HOOK_EXT(CreateXlibSurfaceKHR, VK_KHR_XLIB_SURFACE_EXTENSION_NAME),
	FUEN_INI_HOOK_EXT(CreateXcbSurfaceKHR, VK_KHR_XCB_SURFACE_EXTENSION_NAME),
	FUEN_INI_HOOK_EXT(DestroySurfaceKHR, VK_KHR_SURFACE_EXTENSION_NAME),

	// rp.hpp
	FUEN_DEV_HOOK(CreateFramebuffer),
	FUEN_DEV_HOOK(DestroyFramebuffer),

	FUEN_DEV_HOOK(CreateRenderPass),
	FUEN_DEV_HOOK(DestroyRenderPass),
	FUEN_DEV_HOOK(CreateRenderPass2),
	FUEN_DEV_HOOK_ALIAS(CreateRenderPass2KHR, CreateRenderPass2,
		VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME),

	// image.hpp
	FUEN_DEV_HOOK(CreateImage),
	FUEN_DEV_HOOK(DestroyImage),
	FUEN_DEV_HOOK(BindImageMemory),
	FUEN_DEV_HOOK(BindImageMemory2),
	FUEN_DEV_HOOK_ALIAS(BindImageMemory2KHR, BindImageMemory2,
		VK_KHR_BIND_MEMORY_2_EXTENSION_NAME),

	FUEN_DEV_HOOK(CreateImageView),
	FUEN_DEV_HOOK(DestroyImageView),

	FUEN_DEV_HOOK(CreateSampler),
	FUEN_DEV_HOOK(DestroySampler),

	// buffer.hpp
	FUEN_DEV_HOOK(CreateBuffer),
	FUEN_DEV_HOOK(DestroyBuffer),
	FUEN_DEV_HOOK(BindBufferMemory),
	FUEN_DEV_HOOK(BindBufferMemory2),
	FUEN_DEV_HOOK_ALIAS(BindBufferMemory2KHR, BindBufferMemory2,
		VK_KHR_BIND_MEMORY_2_EXTENSION_NAME),

	FUEN_DEV_HOOK(CreateBufferView),
	FUEN_DEV_HOOK(DestroyBufferView),

	// memory.hpp
	FUEN_DEV_HOOK(AllocateMemory),
	FUEN_DEV_HOOK(FreeMemory),
	FUEN_DEV_HOOK(MapMemory),
	FUEN_DEV_HOOK(UnmapMemory),

	// shader.hpp
	FUEN_DEV_HOOK(CreateShaderModule),
	FUEN_DEV_HOOK(DestroyShaderModule),

	// sync.hpp
	FUEN_DEV_HOOK(CreateFence),
	FUEN_DEV_HOOK(DestroyFence),
	FUEN_DEV_HOOK(ResetFences),
	FUEN_DEV_HOOK(GetFenceStatus),
	FUEN_DEV_HOOK(WaitForFences),

	FUEN_DEV_HOOK(CreateSemaphore),
	FUEN_DEV_HOOK(DestroySemaphore),

	FUEN_DEV_HOOK(CreateEvent),
	FUEN_DEV_HOOK(DestroyEvent),
	FUEN_DEV_HOOK(SetEvent),
	FUEN_DEV_HOOK(ResetEvent),
	FUEN_DEV_HOOK(GetEventStatus),

	// ds.hpp
	FUEN_DEV_HOOK(CreateDescriptorSetLayout),
	FUEN_DEV_HOOK(DestroyDescriptorSetLayout),
	FUEN_DEV_HOOK(CreateDescriptorPool),
	FUEN_DEV_HOOK(DestroyDescriptorPool),
	FUEN_DEV_HOOK(ResetDescriptorPool),
	FUEN_DEV_HOOK(AllocateDescriptorSets),
	FUEN_DEV_HOOK(FreeDescriptorSets),
	FUEN_DEV_HOOK(UpdateDescriptorSets),

	FUEN_DEV_HOOK(CreateDescriptorUpdateTemplate),
	FUEN_DEV_HOOK(DestroyDescriptorUpdateTemplate),
	FUEN_DEV_HOOK(UpdateDescriptorSetWithTemplate),
	FUEN_DEV_HOOK_ALIAS(CreateDescriptorUpdateTemplateKHR,
		CreateDescriptorUpdateTemplate, VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME),
	FUEN_DEV_HOOK_ALIAS(DestroyDescriptorUpdateTemplateKHR,
		DestroyDescriptorUpdateTemplate, VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME),
	FUEN_DEV_HOOK_ALIAS(UpdateDescriptorSetWithTemplateKHR,
		UpdateDescriptorSetWithTemplate, VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME),

	// pipe.hpp
	FUEN_DEV_HOOK(CreateGraphicsPipelines),
	FUEN_DEV_HOOK(CreateComputePipelines),
	FUEN_DEV_HOOK(DestroyPipeline),
	FUEN_DEV_HOOK(CreatePipelineLayout),
	FUEN_DEV_HOOK(DestroyPipelineLayout),

	// queryPool.hpp
	FUEN_DEV_HOOK(CreateQueryPool),
	FUEN_DEV_HOOK(DestroyQueryPool),

	// cb.hpp
	FUEN_DEV_HOOK(CreateCommandPool),
	FUEN_DEV_HOOK(DestroyCommandPool),
	FUEN_DEV_HOOK(ResetCommandPool),

	FUEN_DEV_HOOK(TrimCommandPool),
	FUEN_DEV_HOOK_ALIAS(TrimCommandPoolKHR, TrimCommandPool,
		VK_KHR_MAINTENANCE1_EXTENSION_NAME),

	FUEN_DEV_HOOK(AllocateCommandBuffers),
	FUEN_DEV_HOOK(FreeCommandBuffers),
	FUEN_DEV_HOOK(BeginCommandBuffer),
	FUEN_DEV_HOOK(EndCommandBuffer),
	FUEN_DEV_HOOK(ResetCommandBuffer),

	FUEN_DEV_HOOK(CmdBeginRenderPass),
	FUEN_DEV_HOOK(CmdEndRenderPass),
	FUEN_DEV_HOOK(CmdNextSubpass),
	FUEN_DEV_HOOK(CmdWaitEvents),
	FUEN_DEV_HOOK(CmdPipelineBarrier),
	FUEN_DEV_HOOK(CmdBeginQuery),
	FUEN_DEV_HOOK(CmdEndQuery),
	FUEN_DEV_HOOK(CmdResetQueryPool),
	FUEN_DEV_HOOK(CmdWriteTimestamp),
	FUEN_DEV_HOOK(CmdCopyQueryPoolResults),
	FUEN_DEV_HOOK(CmdDraw),
	FUEN_DEV_HOOK(CmdDrawIndexed),
	FUEN_DEV_HOOK(CmdDrawIndirect),
	FUEN_DEV_HOOK(CmdDrawIndexedIndirect),
	FUEN_DEV_HOOK(CmdDispatch),
	FUEN_DEV_HOOK(CmdDispatchIndirect),
	FUEN_DEV_HOOK(CmdBindVertexBuffers),
	FUEN_DEV_HOOK(CmdBindIndexBuffer),
	FUEN_DEV_HOOK(CmdBindDescriptorSets),
	FUEN_DEV_HOOK(CmdClearColorImage),
	FUEN_DEV_HOOK(CmdClearDepthStencilImage),
	FUEN_DEV_HOOK(CmdClearAttachments),
	FUEN_DEV_HOOK(CmdResolveImage),
	FUEN_DEV_HOOK(CmdSetEvent),
	FUEN_DEV_HOOK(CmdResetEvent),
	FUEN_DEV_HOOK(CmdCopyBufferToImage),
	FUEN_DEV_HOOK(CmdCopyImageToBuffer),
	FUEN_DEV_HOOK(CmdBlitImage),
	FUEN_DEV_HOOK(CmdCopyImage),
	FUEN_DEV_HOOK(CmdExecuteCommands),
	FUEN_DEV_HOOK(CmdCopyBuffer),
	FUEN_DEV_HOOK(CmdFillBuffer),
	FUEN_DEV_HOOK(CmdUpdateBuffer),
	FUEN_DEV_HOOK(CmdBindPipeline),
	FUEN_DEV_HOOK(CmdPushConstants),
	FUEN_DEV_HOOK(CmdSetViewport),
	FUEN_DEV_HOOK(CmdSetScissor),
	FUEN_DEV_HOOK(CmdSetLineWidth),
	FUEN_DEV_HOOK(CmdSetDepthBias),
	FUEN_DEV_HOOK(CmdSetBlendConstants),
	FUEN_DEV_HOOK(CmdSetDepthBounds),
	FUEN_DEV_HOOK(CmdSetStencilCompareMask),
	FUEN_DEV_HOOK(CmdSetStencilWriteMask),
	FUEN_DEV_HOOK(CmdSetStencilReference),

	FUEN_DEV_HOOK(CmdDispatchBase),
	FUEN_DEV_HOOK_ALIAS(CmdDispatchBaseKHR, CmdDispatchBase,
		VK_KHR_DEVICE_GROUP_EXTENSION_NAME),

	FUEN_DEV_HOOK(CmdDrawIndirectCount),
	FUEN_DEV_HOOK_ALIAS(CmdDrawIndirectCountKHR, CmdDrawIndirectCount,
		VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME),
	FUEN_DEV_HOOK_ALIAS(CmdDrawIndirectCountAMD, CmdDrawIndirectCount,
		VK_AMD_DRAW_INDIRECT_COUNT_EXTENSION_NAME),

	FUEN_DEV_HOOK(CmdDrawIndexedIndirectCount),
	FUEN_DEV_HOOK_ALIAS(CmdDrawIndexedIndirectCountKHR, CmdDrawIndexedIndirectCount,
		VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME),
	FUEN_DEV_HOOK_ALIAS(CmdDrawIndexedIndirectCountAMD, CmdDrawIndexedIndirectCount,
		VK_AMD_DRAW_INDIRECT_COUNT_EXTENSION_NAME),

	FUEN_DEV_HOOK(CmdBeginRenderPass2),
	FUEN_DEV_HOOK_ALIAS(CmdBeginRenderPass2KHR, CmdBeginRenderPass2,
		VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME),

	FUEN_DEV_HOOK(CmdNextSubpass2),
	FUEN_DEV_HOOK_ALIAS(CmdNextSubpass2KHR, CmdNextSubpass2,
		VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME),

	FUEN_DEV_HOOK(CmdEndRenderPass2),
	FUEN_DEV_HOOK_ALIAS(CmdEndRenderPass2KHR, CmdEndRenderPass2,
		VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME),
};

#undef FUEN_INI_HOOK
#undef FUEN_INI_HOOK_EXT

#undef FUEN_DEV_HOOK
#undef FUEN_DEV_HOOK_EXT
#undef FUEN_DEV_HOOK_ALIAS

// We make sure this way that e.g. calling vkGetInstanceProcAddr with
// vkGetInstanceProcAddr as funcName parameter returns itself.
// Not sure how important that is though.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance ini, const char* funcName) {
	// Check if we hooked it. If we didn't hook it and ini is invalid,
	// return nullptr.
	auto it = funcPtrTable.find(std::string_view(funcName));
	if(it == funcPtrTable.end()) {
		// If it's not hooked, just forward it to the next chain link
		auto* inid = fuen::findData<fuen::Instance>(ini);
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

	auto* inid = fuen::findData<fuen::Instance>(ini);
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
		auto* dev = fuen::findData<fuen::Device>(vkDev);
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

	auto* dev = fuen::findData<fuen::Device>(vkDev);
	if(!dev) {
		dlg_error("invalid device data: {}", vkDev);
		return nullptr;
	}

	if(!hooked.devExt.empty()) {
		auto it = find(dev->extensions, hooked.devExt);
		if(it == dev->extensions.end()) {
			// dlg_trace("tried to load dev proc addr {} for disabled ext {}",
			// 	funcName, hooked.devExt);
			// TODO: not sure what is better
			return dev->dispatch.GetDeviceProcAddr(vkDev, funcName);
			// return nullptr;
		}
	}

	return hooked.func;
}

} // namespace fuen

// TODO: not sure why we need this on windows
#ifdef _WIN32
	#define FUEN_EXPORT __declspec(dllexport)
#else
	#define FUEN_EXPORT VK_LAYER_EXPORT
#endif

// Global layer entry points
extern "C" FUEN_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance ini, const char* funcName) {
	// dlg_trace("fuen get ini proc addr");
	return fuen::GetInstanceProcAddr(ini, funcName);
}

extern "C" FUEN_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice dev, const char* funcName) {
	// dlg_trace("fuen get device proc addr");
	return fuen::GetDeviceProcAddr(dev, funcName);
}
