#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <string>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>
#include <unordered_map>

#include <dlg/dlg.hpp>

namespace fuen {

struct Instance {
	struct {
		PFN_vkDestroyInstance destroyInstance;
		PFN_vkGetInstanceProcAddr getInstanceProcAddr;
	} vtable;
};

struct Device {
};

// Util
static std::unordered_map<std::uint64_t, void*> dataTable;

template<typename T>
std::uint64_t handleCast(T handle) {
	std::uint64_t id {};
	std::memcpy(&id, &handle, sizeof(handle));
	return id;
}

template<typename T>
void* findData(T handle) {
	auto it = dataTable.find(handleCast(handle));
	return it == dataTable.end() ? nullptr : it->second;
}

template<typename R, typename T>
R* findData(T handle) {
	auto it = dataTable.find(handleCast(handle));
	return static_cast<R*>(it == dataTable.end() ? nullptr : it->second);
}

template<typename T>
void insertData(T handle, void* data) {
	auto [_, success] = dataTable.emplace(handleCast(handle), data);
	dlg_assert(success);
}

template<typename R, typename T, typename... Args>
R& createData(T handle, Args&&... args) {
	auto data = new R(std::forward<Args>(args)...);
	insertData(handle, data);
	return *data;
}

template<typename T>
void eraseData(T handle) {
	auto it = dataTable.find(handleCast(handle));
	dlg_assert_or(it != dataTable.end(), return);
	dataTable.erase(it);
}

template<typename R, typename T>
std::unique_ptr<R> moveData(T handle) {
	auto it = dataTable.find(handleCast(handle));
	dlg_assert_or(it != dataTable.end(), return nullptr);
	auto ptr = it->second;
	dataTable.erase(it);
	return std::unique_ptr<R>(static_cast<R*>(ptr));
}

template<typename R, VkStructureType SType, typename CI>
const R* findChainInfo(const CI& ci) {
	auto* link = static_cast<const VkBaseInStructure*>(ci.pNext);
	while(link) {
		if(link->sType == SType) {
			return reinterpret_cast<const R*>(link);
		}

		link = static_cast<const VkBaseInStructure*>(link->pNext);
	}

	return nullptr;
}

// Hooks
/*
static VkResult CreateDevice(
		VkPhysicalDevice phdev,
		const VkDeviceCreateInfo* ci,
		const VkAllocationCallbacks* alloc,
		VkDevice* dev) {

}
*/

VkResult CreateInstance(
		const VkInstanceCreateInfo* ci,
		const VkAllocationCallbacks* alloc,
		VkInstance* ini) {
	auto* linkInfo = findChainInfo<VkLayerInstanceCreateInfo, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO>(*ci);
	while(linkInfo && linkInfo->function != VK_LAYER_LINK_INFO) {
		linkInfo = findChainInfo<VkLayerInstanceCreateInfo, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO>(*linkInfo);
	}

	dlg_assert(linkInfo);

	auto fpGetInstanceProcAddr = linkInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	auto fpCreateInstance = (PFN_vkCreateInstance)fpGetInstanceProcAddr(NULL, "vkCreateInstance");
	if(!fpCreateInstance) {
		dlg_error("Can't load vkCreateInstance from chain");
		return VK_ERROR_INITIALIZATION_FAILED;
	}

  	// Advance the link info for the next element on the chain
	auto mutLinkInfo = const_cast<VkLayerInstanceCreateInfo*>(linkInfo);
   	mutLinkInfo->u.pLayerInfo = linkInfo->u.pLayerInfo->pNext;

	VkResult result = fpCreateInstance(ci, alloc, ini);
	if(result != VK_SUCCESS) {
		return result;
	}

	auto& instanceData = createData<Instance>(*ini);
	instanceData.vtable.destroyInstance = (PFN_vkDestroyInstance)fpGetInstanceProcAddr(NULL, "vkDestroyInstance");
	instanceData.vtable.getInstanceProcAddr = fpGetInstanceProcAddr;
	dlg_assert(instanceData.vtable.destroyInstance);

	dlg_info("created instance {}{}", std::hex, handleCast(*ini));

	return result;
}

void DestroyInstance(VkInstance ini, const VkAllocationCallbacks* alloc) {
	auto inid = moveData<Instance>(ini);
	dlg_assert(inid);

	inid->vtable.destroyInstance(ini, alloc);
	dlg_info("destroyed instance {}{}", std::hex, handleCast(ini));
}

static const std::unordered_map<std::string_view, void*> funcPtrTable {
   // { "vkGetDeviceProcAddr", (void *) vkGetDeviceProcAddr },
#define ADD_HOOK(fn) { "vk" # fn, (void *) fn }
#define ADD_ALIAS_HOOK(alias, fn) { "vk" # alias, (void *) overlay_ ## fn }
   /*
   ADD_HOOK(AllocateCommandBuffers),
   ADD_HOOK(FreeCommandBuffers),
   ADD_HOOK(ResetCommandBuffer),
   ADD_HOOK(BeginCommandBuffer),
   ADD_HOOK(EndCommandBuffer),
   ADD_HOOK(CmdExecuteCommands),

   ADD_HOOK(CmdDraw),
   ADD_HOOK(CmdDrawIndexed),
   ADD_HOOK(CmdDrawIndirect),
   ADD_HOOK(CmdDrawIndexedIndirect),
   ADD_HOOK(CmdDispatch),
   ADD_HOOK(CmdDispatchIndirect),
   ADD_HOOK(CmdDrawIndirectCount),
   ADD_ALIAS_HOOK(CmdDrawIndirectCountKHR, CmdDrawIndirectCount),
   ADD_HOOK(CmdDrawIndexedIndirectCount),
   ADD_ALIAS_HOOK(CmdDrawIndexedIndirectCountKHR, CmdDrawIndexedIndirectCount),

   ADD_HOOK(CmdBindPipeline),

   ADD_HOOK(CreateSwapchainKHR),
   ADD_HOOK(QueuePresentKHR),
   ADD_HOOK(DestroySwapchainKHR),
   ADD_HOOK(AcquireNextImageKHR),
   ADD_HOOK(AcquireNextImage2KHR),

   ADD_HOOK(QueueSubmit),

   ADD_HOOK(CreateDevice),
   ADD_HOOK(DestroyDevice),
   */

   ADD_HOOK(CreateInstance),
   ADD_HOOK(DestroyInstance),
#undef ADD_HOOK
};

static PFN_vkVoidFunction findFunctionPtr(const char* name) {
	auto it = funcPtrTable.find(std::string_view(name));
	if(it == funcPtrTable.end()) {
		return nullptr;
	}

   return reinterpret_cast<PFN_vkVoidFunction>(it->second);
}

} // namespace fuen

// Global layer entry points
/*
extern "C" VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice dev, const char* funcName) {
   auto ptr = fuen::findFunctionPtr(funcName);
   if(ptr || !dev) {
	   return ptr;
   }

   auto* devd = fuen::findData<fuen::Device>(dev);
   if(!devd) {
	   return nullptr;
   }

   // TODO
   return nullptr;
}
*/

extern "C" VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance ini, const char* funcName) {
   auto ptr = fuen::findFunctionPtr(funcName);
   if(ptr || !ini) {
	   return ptr;
   }

   auto* inid = fuen::findData<fuen::Instance>(ini);
   if(!inid || !inid->vtable.getInstanceProcAddr) {
	   dlg_error("could not find instance data: {}", fuen::handleCast(ini));
	   return nullptr;
   }

   return inid->vtable.getInstanceProcAddr(ini, funcName);
}

