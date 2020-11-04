#include "common.hpp"
#include "data.hpp"
#include "wayland.hpp"

#include "image.hpp"
#include "rp.hpp"
#include "cb.hpp"

#include <vkpp/enums.hpp>
#include <vkpp/names.hpp>
#include <vkpp/dispatch.hpp>
#include <vkpp/dispatch.cpp>
#include <vkpp/span.hpp>

#include <dlg/dlg.hpp>

#include <vulkan/vk_layer.h>

#include <string>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>

#include <imgui/imgui.h>

thread_local ImGuiContext* __LayerImGui;

namespace fuen {

// data.hpp
std::unordered_map<std::uint64_t, void*> dataTable;
std::shared_mutex dataMutex;

// high-level stuff
u32 findLSB(u32 v) {
	// https://stackoverflow.com/questions/757059
	static const int blackMagic[32] = {
		0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
		31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
	};
	return blackMagic[((u32)((v & -v) * 0x077CB531U)) >> 27];
}

void Draw::init(Device& dev) {
	// init data
	VkCommandBufferAllocateInfo cbai = vk::CommandBufferAllocateInfo();
	cbai.commandBufferCount = 1u;
	cbai.commandPool = dev.commandPool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	VK_CHECK(dev.dispatch.vkAllocateCommandBuffers(dev.dev, &cbai, &cb));

	// command buffer is a dispatchable object
	dev.setDeviceLoaderData(dev.dev, cb);

	VkFenceCreateInfo fci = vk::FenceCreateInfo();
	VK_CHECK(dev.dispatch.vkCreateFence(dev.dev, &fci, nullptr, &fence));

	VkSemaphoreCreateInfo sci = vk::SemaphoreCreateInfo();
	VK_CHECK(dev.dispatch.vkCreateSemaphore(dev.dev, &sci, nullptr, &semaphore));

	VkDescriptorSetAllocateInfo dsai = vk::DescriptorSetAllocateInfo();
	dsai.descriptorPool = dev.dsPool;
	dsai.descriptorSetCount = 1u;
	dsai.pSetLayouts = &dev.dsLayout;
	VK_CHECK(dev.dispatch.vkAllocateDescriptorSets(dev.dev, &dsai, &ds));
}

void RenderBuffer::init(Device& dev, VkImage img, VkFormat format,
		VkExtent2D extent, VkRenderPass rp) {
	this->dev = &dev;
	this->image = img;

	VkImageViewCreateInfo ivi = vk::ImageViewCreateInfo();
	ivi.image = image;
	ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ivi.components = {};
	ivi.format = format;
	ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	ivi.subresourceRange.layerCount = 1;
	ivi.subresourceRange.levelCount = 1;
	VK_CHECK(dev.dispatch.vkCreateImageView(dev.dev, &ivi, nullptr, &view));

	VkFramebufferCreateInfo fbi = vk::FramebufferCreateInfo();
	fbi.attachmentCount = 1u;
	fbi.pAttachments = &view;
	fbi.layers = 1u;
	fbi.width = extent.width;
	fbi.height = extent.height;
	fbi.renderPass = rp;
	VK_CHECK(dev.dispatch.vkCreateFramebuffer(dev.dev, &fbi, nullptr, &fb));
}

RenderBuffer::~RenderBuffer() {
	if(dev) {
		if(fb) {
			dev->dispatch.vkDestroyFramebuffer(dev->dev, fb, nullptr);
		}
		if(view) {
			dev->dispatch.vkDestroyImageView(dev->dev, view, nullptr);
		}
	}
}

void Draw::Buffer::ensure(Device& dev, VkDeviceSize reqSize,
		VkBufferUsageFlags usage) {
	if(size >= reqSize) {
		return;
	}

	// over-allocate
	reqSize *= 2;

	if(buf) {
		dev.dispatch.vkDestroyBuffer(dev.dev, buf, nullptr);
		dev.dispatch.vkFreeMemory(dev.dev, mem, nullptr);
	}

	// new buffer
	VkBufferCreateInfo bufInfo = vk::BufferCreateInfo();
	bufInfo.size = reqSize;
	bufInfo.usage = usage;
	VK_CHECK(dev.dispatch.vkCreateBuffer(dev.dev, &bufInfo, nullptr, &buf));

	// get memory props
	VkMemoryRequirements memReqs;
	dev.dispatch.vkGetBufferMemoryRequirements(dev.dev, buf, &memReqs);

	// new memory
	VkMemoryAllocateInfo allocInfo = vk::MemoryAllocateInfo();
	allocInfo.allocationSize = memReqs.size;
	allocInfo.memoryTypeIndex = findLSB(memReqs.memoryTypeBits & dev.hostVisibleMemTypeBits);
	VK_CHECK(dev.dispatch.vkAllocateMemory(dev.dev, &allocInfo, nullptr, &mem));

	// bind
	VK_CHECK(dev.dispatch.vkBindBufferMemory(dev.dev, buf, mem, 0));
	this->size = reqSize;
}

// util
template<typename R, VkStructureType SType, typename CI>
const R* findChainInfo(const CI& ci) {
	auto* link = static_cast<const VkBaseInStructure*>(ci.pNext);
	// dlg_trace("pNext chain looking on {} for {}", &ci, vk::name(vk::StructureType(SType)));
	while(link) {
		// dlg_trace("\tpNext: {}", vk::name(vk::StructureType(link->sType)));
		if(link->sType == SType) {
			return reinterpret_cast<const R*>(link);
		}

		link = static_cast<const VkBaseInStructure*>(link->pNext);
	}

	return nullptr;
}

// Hooks
// Instance
VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(
		const VkInstanceCreateInfo* ci,
		const VkAllocationCallbacks* alloc,
		VkInstance* ini) {
	auto* linkInfo = findChainInfo<VkLayerInstanceCreateInfo, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO>(*ci);
	while(linkInfo && linkInfo->function != VK_LAYER_LINK_INFO) {
		linkInfo = findChainInfo<VkLayerInstanceCreateInfo, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO>(*linkInfo);
	}

	if(!linkInfo) {
		dlg_error("No linkInfo");
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	auto fpGetInstanceProcAddr = linkInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	auto fpCreateInstance = (PFN_vkCreateInstance)fpGetInstanceProcAddr(NULL, "vkCreateInstance");
	if(!fpCreateInstance) {
		dlg_error("could not load vkCreateInstance");
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
	instanceData.ini = *ini;

	instanceData.dispatch.init((vk::Instance) *ini, fpGetInstanceProcAddr, false);
	// Needed in case the next layer does not make vkGetInstanceProcAddr
	// return itself correctly.
	instanceData.dispatch.vkGetInstanceProcAddr = fpGetInstanceProcAddr;

	// add instance data to all physical devices so we can retrieve
	// it in CreateDevice
	u32 phdevCount = 0;
	instanceData.dispatch.vkEnumeratePhysicalDevices(*ini, &phdevCount, nullptr);
	auto phdevs = std::make_unique<VkPhysicalDevice[]>(phdevCount);
	instanceData.dispatch.vkEnumeratePhysicalDevices(*ini, &phdevCount, phdevs.get());

	for(auto i = 0u; i < phdevCount; ++i) {
		insertData(phdevs[i], &instanceData);
	}

	return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance ini, const VkAllocationCallbacks* alloc) {
	auto inid = moveData<Instance>(ini);
	dlg_assert(inid);

	inid->dispatch.vkDestroyInstance(ini, alloc);
}

// Device
VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(
		VkPhysicalDevice phdev,
		const VkDeviceCreateInfo* ci,
		const VkAllocationCallbacks* alloc,
		VkDevice* dev) {
	auto* iniData = findData<Instance>(phdev);
	dlg_assert(iniData);

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
	auto fpCreateDevice = (PFN_vkCreateDevice)fpGetInstanceProcAddr(iniData->ini, "vkCreateDevice");
	if(!fpCreateDevice) {
		dlg_error("could not load vkCreateDevice");
		return VK_ERROR_INITIALIZATION_FAILED;
	}

  	// Advance the link info for the next element on the chain
	auto mutLinkInfo = const_cast<VkLayerDeviceCreateInfo*>(linkInfo);
   	mutLinkInfo->u.pLayerInfo = linkInfo->u.pLayerInfo->pNext;

	VkResult result = fpCreateDevice(phdev, ci, alloc, dev);
	if(result != VK_SUCCESS) {
		return result;
	}

	auto& devData = createData<Device>(*dev);
	devData.ini = iniData;
	devData.phdev = phdev;
	devData.dev = *dev;
	devData.dispatch = iniData->dispatch;
	devData.dispatch.vkGetInstanceProcAddr = fpGetInstanceProcAddr;
	devData.dispatch.vkGetDeviceProcAddr = fpGetDeviceProcAddr;
	devData.dispatch.init((vk::Device) *dev);
	// In case GetDeviceProcAddr of the next chain didn't return itself
	devData.dispatch.vkGetDeviceProcAddr = fpGetDeviceProcAddr;

	devData.swapchains.mutex = &devData.mutex;
	devData.images.mutex = &devData.mutex;
	devData.imageViews.mutex = &devData.mutex;
	devData.buffers.mutex = &devData.mutex;
	devData.framebuffers.mutex = &devData.mutex;
	devData.renderPasses.mutex = &devData.mutex;
	devData.commandBuffers.mutex = &devData.mutex;
	devData.commandPools.mutex = &devData.mutex;

	// find vkSetDeviceLoaderData callback
	auto* loaderData = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*ci);
	while(loaderData && loaderData->function != VK_LOADER_DATA_CALLBACK) {
		loaderData = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*loaderData);
	}

	dlg_assert(loaderData);
	devData.setDeviceLoaderData = loaderData->u.pfnSetDeviceLoaderData;

   	// query queues
	u32 nqf;
	devData.dispatch.vkGetPhysicalDeviceQueueFamilyProperties(phdev, &nqf, nullptr);
	auto qfprops = std::make_unique<VkQueueFamilyProperties[]>(nqf);
	devData.dispatch.vkGetPhysicalDeviceQueueFamilyProperties(phdev, &nqf, qfprops.get());

	// reserve vector so we don't get a reallocation
	auto nQueues = 0u;
	for(auto i = 0u; i < ci->queueCreateInfoCount; ++i) {
		nQueues += ci->pQueueCreateInfos[i].queueCount;
	}

	devData.queues.reserve(nQueues);
	for(auto i = 0u; i < ci->queueCreateInfoCount; ++i) {
		auto& qi = ci->pQueueCreateInfos[i];
		auto& familyProps = qfprops[qi.queueFamilyIndex];

		for(auto j = 0u; j < qi.queueCount; ++j) {
			auto& q = devData.queues.emplace_back();
			q.dev = &devData;
			q.flags = familyProps.queueFlags;
			devData.dispatch.vkGetDeviceQueue(*dev, qi.queueFamilyIndex, j, &q.queue);

			// Queue is a dispatchable handle.
			// We therefore have to inform the loader that we created this
			// resource inside the layer and let it set its dispatch table.
			// We will also have to get our queue-data just from the VkQueue
			// later on (e.g. vkQueueSubmit) so associate data with it.
			devData.setDeviceLoaderData(*dev, q.queue);
			insertData(q.queue, &devData.queues.back());

			if(!devData.gfxQueue && familyProps.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
				devData.gfxQueue = &devData.queues.back();
			}
		}
	}

	// query memory stuff
	VkPhysicalDeviceMemoryProperties memProps;
	devData.dispatch.vkGetPhysicalDeviceMemoryProperties(phdev, &memProps);
	for(auto i = 0u; i < memProps.memoryTypeCount; ++i) {
		auto flags = memProps.memoryTypes[i].propertyFlags;
		if(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
			devData.hostVisibleMemTypeBits |= (1 << i);
		}

		if(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
			devData.deviceLocalMemTypeBits |= (1 << i);
		}
	}

	if(devData.gfxQueue) {
		// sampler
		VkSamplerCreateInfo sci = vk::SamplerCreateInfo();
		sci.magFilter = VK_FILTER_LINEAR;
		sci.minFilter = VK_FILTER_LINEAR;
		sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sci.minLod = -1000;
		sci.maxLod = 1000;
		sci.maxAnisotropy = 1.0f;
		VK_CHECK(devData.dispatch.vkCreateSampler(*dev, &sci, nullptr, &devData.sampler));

		// command pool
		VkCommandPoolCreateInfo cpci = vk::CommandPoolCreateInfo();
		cpci.queueFamilyIndex = devData.gfxQueue->family;
		cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		VK_CHECK(devData.dispatch.vkCreateCommandPool(*dev, &cpci, nullptr, &devData.commandPool));

		// descriptor set layout
		VkDescriptorSetLayoutBinding binding;
		binding.binding = 0u;
		binding.descriptorCount = 1u;
		binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		binding.pImmutableSamplers = &devData.sampler;
		binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutCreateInfo dslci = vk::DescriptorSetLayoutCreateInfo();
		dslci.bindingCount = 1u;
		dslci.pBindings = &binding;
		VK_CHECK(devData.dispatch.vkCreateDescriptorSetLayout(*dev, &dslci, nullptr, &devData.dsLayout));

		// pipeline layout
		VkPushConstantRange pcrs[1] = {};
		pcrs[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		pcrs[0].offset = sizeof(float) * 0;
		pcrs[0].size = sizeof(float) * 4;

		VkPipelineLayoutCreateInfo plci = vk::PipelineLayoutCreateInfo();
		plci.setLayoutCount = 1;
		plci.pSetLayouts = &devData.dsLayout;
		plci.pushConstantRangeCount = 1;
		plci.pPushConstantRanges = pcrs;
		VK_CHECK(devData.dispatch.vkCreatePipelineLayout(*dev, &plci, nullptr, &devData.pipeLayout));

		// descriptoer pool
		VkDescriptorPoolSize poolSize;
		poolSize.descriptorCount = 50u;
		poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

		VkDescriptorPoolCreateInfo dpci = vk::DescriptorPoolCreateInfo();
		dpci.pPoolSizes = &poolSize;
		dpci.poolSizeCount = 1u;
		dpci.maxSets = 50u;
		dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		VK_CHECK(devData.dispatch.vkCreateDescriptorPool(*dev, &dpci, nullptr, &devData.dsPool));
	}

	constexpr auto useWindow = true;
	if(useWindow) {
		devData.window.init(devData);
	}

	return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(
		VkDevice dev,
		VkAllocationCallbacks* alloc) {
	auto devd = moveData<Device>(dev);
	dlg_assert(devd);

	// user must have erased all resources
	dlg_assert(devd->swapchains.empty());
	dlg_assert(devd->images.empty());
	dlg_assert(devd->buffers.empty());
	dlg_assert(devd->imageViews.empty());
	dlg_assert(devd->framebuffers.empty());
	dlg_assert(devd->renderPasses.empty());

	// erase queue datas
	for(auto& queue : devd->queues) {
		eraseData(queue.queue);
	}

	// destroy our logical device before we call the function.
	auto* destroyDev = devd->dispatch.vkDestroyDevice;
	devd.release();
	destroyDev(dev, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectNameEXT(
		VkDevice                                    device,
		const VkDebugUtilsObjectNameInfoEXT*        pNameInfo) {
	auto& devd = getData<Device>(device);
	switch(pNameInfo->objectType) {
		case VK_OBJECT_TYPE_QUEUE: {
			// queues is dispatchable
			auto& q = getData<Queue>((VkQueue) pNameInfo->objectHandle);
			q.name = pNameInfo->pObjectName;
			break;
		} case VK_OBJECT_TYPE_SWAPCHAIN_KHR: {
			auto& sc = devd.swapchains.get((VkSwapchainKHR) pNameInfo->objectHandle);
			sc.name = pNameInfo->pObjectName;
			break;
		} case VK_OBJECT_TYPE_IMAGE: {
			auto& img = devd.images.get((VkImage) pNameInfo->objectHandle);
			img.name = pNameInfo->pObjectName;
			break;
		} case VK_OBJECT_TYPE_IMAGE_VIEW: {
			auto& view = devd.imageViews.get((VkImageView) pNameInfo->objectHandle);
			view.name = pNameInfo->pObjectName;
			break;
		} case VK_OBJECT_TYPE_BUFFER: {
			auto& buf = devd.buffers.get((VkBuffer) pNameInfo->objectHandle);
			buf.name = pNameInfo->pObjectName;
			break;
		} case VK_OBJECT_TYPE_FRAMEBUFFER: {
			auto& fb = devd.framebuffers.get((VkFramebuffer) pNameInfo->objectHandle);
			fb.name = pNameInfo->pObjectName;
			break;
		} case VK_OBJECT_TYPE_RENDER_PASS: {
			auto& rp = devd.renderPasses.get((VkRenderPass) pNameInfo->objectHandle);
			rp.name = pNameInfo->pObjectName;
			break;
		} case VK_OBJECT_TYPE_COMMAND_POOL: {
			auto& cp = devd.commandPools.get((VkCommandPool) pNameInfo->objectHandle);
			cp.name = pNameInfo->pObjectName;
			break;
		} case VK_OBJECT_TYPE_COMMAND_BUFFER: {
			auto& cp = devd.commandBuffers.get((VkCommandBuffer) pNameInfo->objectHandle);
			cp.name = pNameInfo->pObjectName;
			break;
		} default: break;
	}

	if(!devd.dispatch.vkSetDebugUtilsObjectNameEXT) {
		return VK_SUCCESS;
	}

	return devd.dispatch.vkSetDebugUtilsObjectNameEXT(device, pNameInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(
		VkDevice                                    device,
		const VkSwapchainCreateInfoKHR*             pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkSwapchainKHR*                             pSwapchain) {
	auto& devd = getData<Device>(device);
	auto result = devd.dispatch.vkCreateSwapchainKHR(device, pCreateInfo,
		pAllocator, pSwapchain);

	if(result != VK_SUCCESS) {
		return result;
	}

	// TODO: try to simply reuse all of pCreateInfo->oldSwapchain.
	auto pswapd = std::make_unique<Swapchain>();
	auto& swapd = *pswapd;
	swapd.dev = &devd;
	swapd.ci = *pCreateInfo;
	swapd.swapchain = *pSwapchain;

	// add swapchain images to tracked images
	u32 imgCount = 0u;
	VK_CHECK(devd.dispatch.vkGetSwapchainImagesKHR(devd.dev, swapd.swapchain, &imgCount, nullptr));
	auto imgs = std::make_unique<VkImage[]>(imgCount);
	VK_CHECK(devd.dispatch.vkGetSwapchainImagesKHR(devd.dev, swapd.swapchain, &imgCount, imgs.get()));

	swapd.images.resize(imgCount);
	for(auto i = 0u; i < imgCount; ++i) {
		auto& img = devd.images.add(imgs[i]);
		img.swapchain = &swapd;
		img.dev = &devd;
		img.image = imgs[i];
		img.name = dlg::format("Swapchain {} image {}", (void*) swapd.swapchain, i);

		swapd.images[i] = &img;
	}

	// overlay?
	constexpr auto overlay = false;
	if(overlay) {
		swapd.useOverlay = true;
		swapd.overlay.init(swapd);
	}

	devd.swapchains.mustEmplace(*pSwapchain, std::move(pswapd));
	return result;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(
		VkDevice                                    device,
		VkSwapchainKHR                             	swapchain,
		const VkAllocationCallbacks*                pAllocator) {
	auto& devd = getData<Device>(device);

	auto& sc = devd.swapchains.get(swapchain);
	for(auto* img : sc.images) {
		devd.images.mustErase(img->image);
	}

	devd.swapchains.mustErase(swapchain);

	devd.dispatch.vkDestroySwapchainKHR(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(
		VkQueue                                     queue,
		const VkPresentInfoKHR*                     pPresentInfo) {
	auto& qd = getData<Queue>(queue);

	auto combinedResult = VK_SUCCESS;
	for(auto i = 0u; i < pPresentInfo->swapchainCount; ++i) {
		auto& swapchain = qd.dev->swapchains.get(pPresentInfo->pSwapchains[i]);
		VkResult res;
		if(swapchain.useOverlay) {
			auto waitsems = std::span{pPresentInfo->pWaitSemaphores, pPresentInfo->waitSemaphoreCount};
			res = swapchain.overlay.drawPresent(qd, waitsems, pPresentInfo->pImageIndices[i]);
		} else {
			VkPresentInfoKHR pi = vk::PresentInfoKHR();
			pi.pImageIndices = &pPresentInfo->pImageIndices[i];
			pi.pResults = pPresentInfo->pResults ? &pPresentInfo->pResults[i] : nullptr;
			pi.pSwapchains = &pPresentInfo->pSwapchains[i];
			pi.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
			pi.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
			pi.swapchainCount = 1u;
			// TODO: might be bad to not forward this
			// pi.pNext

			res = qd.dev->dispatch.vkQueuePresentKHR(queue, &pi);
		}

		if(pPresentInfo->pResults) {
			pPresentInfo->pResults[i] = res;
		}

		if(res != VK_SUCCESS && combinedResult == VK_SUCCESS) {
			combinedResult = res;
		}
	}

	return combinedResult;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(
		VkQueue                                     queue,
		uint32_t                                    submitCount,
		const VkSubmitInfo*                         pSubmits,
		VkFence                                     fence) {
	auto& qd = getData<Queue>(queue);
	auto& dev = *qd.dev;
	return dev.dispatch.vkQueueSubmit(queue, submitCount, pSubmits, fence);
}

VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(
		VkInstance                                  instance,
		VkSurfaceKHR                                surface,
		const VkAllocationCallbacks*                pAllocator) {
	auto platform = moveDataOpt<Platform>(surface); // destroy it
	auto& ini = getData<Instance>(instance);
	ini.dispatch.vkDestroySurfaceKHR(instance, surface, pAllocator);
}

VKAPI_CALL PFN_vkVoidFunction GetInstanceProcAddr(VkInstance, const char*);
VKAPI_CALL PFN_vkVoidFunction GetDeviceProcAddr(VkDevice, const char*);

#define FUEN_HOOK(fn) { "vk" # fn, (void *) fn }
#define FUEN_ALIAS(alias, fn) { "vk" # alias, (void *) ## fn }

static const std::unordered_map<std::string_view, void*> funcPtrTable {
   FUEN_HOOK(GetInstanceProcAddr),
   FUEN_HOOK(GetDeviceProcAddr),

   FUEN_HOOK(CreateInstance),
   FUEN_HOOK(DestroyInstance),

   FUEN_HOOK(CreateDevice),
   FUEN_HOOK(DestroyDevice),

   FUEN_HOOK(CreateSwapchainKHR),
   FUEN_HOOK(DestroySwapchainKHR),

   FUEN_HOOK(QueueSubmit),
   FUEN_HOOK(QueuePresentKHR),

   // TODO: we probably have to implement *all* the functions since
   // we say we support the extension.
   // We probably also have to return nullptr when the extension
   // isn't enabled, instead of our implementations. Just add
   // an extra "(func name -> (extension, func ptr))" lookup table. Or
   // simply add "extension" field here that is empty usually?
   // Anyways, then check (in GetProcAddr) whether that extension was
   // enabled.
   FUEN_HOOK(SetDebugUtilsObjectNameEXT),

   // TODO: make optional
   FUEN_HOOK(CreateWaylandSurfaceKHR),

   FUEN_HOOK(DestroySurfaceKHR),

   // rp.hpp
   FUEN_HOOK(CreateFramebuffer),
   FUEN_HOOK(DestroyFramebuffer),

   FUEN_HOOK(CreateRenderPass),
   FUEN_HOOK(DestroyRenderPass),

   // image.hpp
   FUEN_HOOK(CreateImage),
   FUEN_HOOK(DestroyImage),

   FUEN_HOOK(CreateImageView),
   FUEN_HOOK(DestroyImageView),

   // cb.hpp
   FUEN_HOOK(CreateCommandPool),
   FUEN_HOOK(DestroyCommandPool),
   FUEN_HOOK(ResetCommandPool),

   FUEN_HOOK(AllocateCommandBuffers),
   FUEN_HOOK(FreeCommandBuffers),
   FUEN_HOOK(BeginCommandBuffer),
   FUEN_HOOK(EndCommandBuffer),
   FUEN_HOOK(ResetCommandBuffer),

   FUEN_HOOK(CmdBeginRenderPass),
   FUEN_HOOK(CmdWaitEvents),
   FUEN_HOOK(CmdPipelineBarrier),
};

#undef FUEN_HOOK
#undef FUEN_ALIAS

PFN_vkVoidFunction findFunctionPtr(const char* name) {
	auto it = funcPtrTable.find(std::string_view(name));
	if(it == funcPtrTable.end()) {
		return nullptr;
	}

   return reinterpret_cast<PFN_vkVoidFunction>(it->second);
}

// We make sure this way that e.g. calling vkGetInstanceProcAddr with
// vkGetInstanceProcAddr as funcName parameter returns itself.
// Not sure how important that is though.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance ini, const char* funcName) {
	// Check if we hooked it. If we didn't hook it and ini is invalid,
	// return nullptr.
	auto ptr = fuen::findFunctionPtr(funcName);
	if(ptr || !ini) {
		return ptr;
	}

	// If it's not hooked, just forward it to the next chain link
	auto* inid = fuen::findData<fuen::Instance>(ini);
	if(!inid || !inid->dispatch.vkGetInstanceProcAddr) {
		dlg_error("invalid instance data: {}", fuen::handleCast(ini));
		return nullptr;
	}

	return inid->dispatch.vkGetInstanceProcAddr(ini, funcName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice dev, const char* funcName) {
   auto ptr = fuen::findFunctionPtr(funcName);
   if(ptr || !dev) {
	   return ptr;
   }

   auto* devd = fuen::findData<fuen::Device>(dev);
   if(!devd || !devd->dispatch.vkGetDeviceProcAddr) {
		dlg_error("invalid device data: {}", fuen::handleCast(dev));
	   return nullptr;
   }

   return devd->dispatch.vkGetDeviceProcAddr(dev, funcName);
}

} // namespace fuen

// Global layer entry points
extern "C" VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance ini, const char* funcName) {
	return fuen::GetInstanceProcAddr(ini, funcName);
}

extern "C" VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice dev, const char* funcName) {
	return fuen::GetDeviceProcAddr(dev, funcName);
}
