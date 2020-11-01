#include "common.hpp"
#include "data.hpp"
#include "wayland.hpp"

#include "overlay.frag.spv.h"
#include "overlay.vert.spv.h"

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

// yep, this is bad.
namespace std {
	using nytl::span;
} // namespace std

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
	devData.dev = *dev;
	devData.dispatch = iniData->dispatch;
	devData.dispatch.vkGetInstanceProcAddr = fpGetInstanceProcAddr;
	devData.dispatch.vkGetDeviceProcAddr = fpGetDeviceProcAddr;
	devData.dispatch.init((vk::Device) *dev);
	// In case GetDeviceProcAddr of the next chain didn't return itself
	devData.dispatch.vkGetDeviceProcAddr = fpGetDeviceProcAddr;

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

	// erase queue datas
	for(auto& queue : devd->queues) {
		eraseData(queue.queue);
	}

	// destroy our logical device before we call the function.
	auto* destroyDev = devd->dispatch.vkDestroyDevice;
	devd.release();
	destroyDev(dev, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImage(
		VkDevice                                   	device,
		const VkImageCreateInfo*                    pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkImage*                                    pImage) {
	auto& devd = *findData<Device>(device);

	// TODO: check if sampling is supported for this image
	auto ici = *pCreateInfo;
	ici.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;

	auto res = devd.dispatch.vkCreateImage(device, pCreateInfo, pAllocator, pImage);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto pimg = std::make_unique<Image>();
	auto& img = *pimg;
	img.ci = *pCreateInfo;

	{
		std::lock_guard lock(devd.mutex);
		auto [it, success] = devd.images.emplace(*pImage, std::move(pimg));
		dlg_assert(success);
	}

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyImage(
		VkDevice                                    device,
		VkImage                                     image,
		const VkAllocationCallbacks*                pAllocator) {
	auto& devd = *findData<Device>(device);

	{
		std::lock_guard lock(devd.mutex);
		auto it = devd.images.find(image);
		dlg_assert(it != devd.images.end());
		devd.images.erase(it);
	}

	devd.dispatch.vkDestroyImage(device, image, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectNameEXT(
		VkDevice                                    device,
		const VkDebugUtilsObjectNameInfoEXT*        pNameInfo) {
	auto& devd = getData<Device>(device);
	switch(pNameInfo->objectType) {
		case VK_OBJECT_TYPE_IMAGE: {
			std::shared_lock lock(devd.mutex);
			auto it = devd.images.find((VkImage) pNameInfo->objectHandle);
			dlg_assert(it != devd.images.end());
			it->second->name = pNameInfo->pObjectName;
			break;
		} case VK_OBJECT_TYPE_BUFFER: {
			std::shared_lock lock(devd.mutex);
			auto it = devd.buffers.find((VkBuffer) pNameInfo->objectHandle);
			dlg_assert(it != devd.buffers.end());
			it->second->name = pNameInfo->pObjectName;
			break;
		} default: break;
	}

	if(!devd.dispatch.vkSetDebugUtilsObjectNameEXT) {
		return VK_SUCCESS;
	}

	return devd.dispatch.vkSetDebugUtilsObjectNameEXT(device, pNameInfo);
}

// Swapchain
void setupSwapchain(Swapchain& swapchain) {
	auto& dev = *swapchain.dev;

	// renderpass
	VkAttachmentDescription attachment = {};
	attachment.format = swapchain.ci.imageFormat;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentReference colorAttachment = {};
	colorAttachment.attachment = 0;
	colorAttachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorAttachment;

	VkSubpassDependency dependencies[2] = {};

	// in-dependency
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	// out-dependency
	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

	VkRenderPassCreateInfo rpi = vk::RenderPassCreateInfo();
	rpi.attachmentCount = 1;
	rpi.pAttachments = &attachment;
	rpi.subpassCount = 1;
	rpi.pSubpasses = &subpass;
	rpi.dependencyCount = 2;
	rpi.pDependencies = dependencies;

	VK_CHECK(dev.dispatch.vkCreateRenderPass(dev.dev, &rpi, nullptr, &swapchain.rp));

	// pipeline
	VkShaderModule vertModule, fragModule;

	VkShaderModuleCreateInfo vertShaderInfo = vk::ShaderModuleCreateInfo();
	vertShaderInfo.codeSize = sizeof(overlay_vert_spv_data);
	vertShaderInfo.pCode = overlay_vert_spv_data;
	VK_CHECK(dev.dispatch.vkCreateShaderModule(dev.dev, &vertShaderInfo, NULL, &vertModule));

	VkShaderModuleCreateInfo fragShaderInfo = vk::ShaderModuleCreateInfo();
	fragShaderInfo.codeSize = sizeof(overlay_frag_spv_data);
	fragShaderInfo.pCode = overlay_frag_spv_data;
	VK_CHECK(dev.dispatch.vkCreateShaderModule(dev.dev, &fragShaderInfo, NULL, &fragModule));

	VkPipelineShaderStageCreateInfo stage[2] = {};
	stage[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stage[0].module = vertModule;
	stage[0].pName = "main";

	stage[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stage[1].module = fragModule;
	stage[1].pName = "main";

	VkVertexInputBindingDescription bindingDesc[1] = {};
	bindingDesc[0].stride = sizeof(ImDrawVert);
	bindingDesc[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkVertexInputAttributeDescription attribDesc[3] = {};
	attribDesc[0].location = 0;
	attribDesc[0].binding = bindingDesc[0].binding;
	attribDesc[0].format = VK_FORMAT_R32G32_SFLOAT;
	attribDesc[0].offset = offsetof(ImDrawVert, pos);

	attribDesc[1].location = 1;
	attribDesc[1].binding = bindingDesc[0].binding;
	attribDesc[1].format = VK_FORMAT_R32G32_SFLOAT;
	attribDesc[1].offset = offsetof(ImDrawVert, uv);

	attribDesc[2].location = 2;
	attribDesc[2].binding = bindingDesc[0].binding;
	attribDesc[2].format = VK_FORMAT_R8G8B8A8_UNORM;
	attribDesc[2].offset = offsetof(ImDrawVert, col);

	VkPipelineVertexInputStateCreateInfo vertexInfo = vk::PipelineVertexInputStateCreateInfo();
	vertexInfo.vertexBindingDescriptionCount = 1;
	vertexInfo.pVertexBindingDescriptions = bindingDesc;
	vertexInfo.vertexAttributeDescriptionCount = 3;
	vertexInfo.pVertexAttributeDescriptions = attribDesc;

	VkPipelineInputAssemblyStateCreateInfo iaInfo = vk::PipelineInputAssemblyStateCreateInfo();
	iaInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportInfo = vk::PipelineViewportStateCreateInfo();
	viewportInfo.viewportCount = 1;
	viewportInfo.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasterInfo = vk::PipelineRasterizationStateCreateInfo();
	rasterInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterInfo.polygonMode = VK_POLYGON_MODE_FILL;
	rasterInfo.cullMode = VK_CULL_MODE_NONE;
	rasterInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterInfo.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo msInfo = vk::PipelineMultisampleStateCreateInfo();
	msInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	msInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineColorBlendAttachmentState colorAttach[1] = {};
	colorAttach[0].blendEnable = VK_TRUE;
	colorAttach[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	colorAttach[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	colorAttach[0].colorBlendOp = VK_BLEND_OP_ADD;
	colorAttach[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	colorAttach[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	colorAttach[0].alphaBlendOp = VK_BLEND_OP_ADD;
	colorAttach[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
		VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	VkPipelineDepthStencilStateCreateInfo depthInfo = vk::PipelineDepthStencilStateCreateInfo();

	VkPipelineColorBlendStateCreateInfo blendInfo = vk::PipelineColorBlendStateCreateInfo();
	blendInfo.attachmentCount = 1;
	blendInfo.pAttachments = colorAttach;

	VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic_state = vk::PipelineDynamicStateCreateInfo();
	dynamic_state.dynamicStateCount = 2;
	dynamic_state.pDynamicStates = dynStates;

	VkGraphicsPipelineCreateInfo gpi = {};
	gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	gpi.flags = 0;
	gpi.stageCount = 2;
	gpi.pStages = stage;
	gpi.pVertexInputState = &vertexInfo;
	gpi.pInputAssemblyState = &iaInfo;
	gpi.pViewportState = &viewportInfo;
	gpi.pRasterizationState = &rasterInfo;
	gpi.pMultisampleState = &msInfo;
	gpi.pDepthStencilState = &depthInfo;
	gpi.pColorBlendState = &blendInfo;
	gpi.pDynamicState = &dynamic_state;
	gpi.layout = dev.pipeLayout;
	gpi.renderPass = swapchain.rp;
	VK_CHECK(dev.dispatch.vkCreateGraphicsPipelines(dev.dev,
		VK_NULL_HANDLE, 1, &gpi, nullptr, &swapchain.pipe));

	dev.dispatch.vkDestroyShaderModule(dev.dev, vertModule, nullptr);
	dev.dispatch.vkDestroyShaderModule(dev.dev, fragModule, nullptr);
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
	swapd.platform = findData<Platform>(pCreateInfo->surface);

	// setup swapchain
	// setup render pass and pipeline
	setupSwapchain(swapd);

	u32 imgCount = 0u;
	VK_CHECK(devd.dispatch.vkGetSwapchainImagesKHR(device, *pSwapchain, &imgCount, nullptr));
	auto imgs = std::make_unique<VkImage[]>(imgCount);
	VK_CHECK(devd.dispatch.vkGetSwapchainImagesKHR(device, *pSwapchain, &imgCount, imgs.get()));

	swapd.buffers.resize(imgCount);
	for(auto i = 0u; i < imgCount; ++i) {
		auto& buf = swapd.buffers[i];
		buf.image = imgs[i];

		VkImageViewCreateInfo ivi = vk::ImageViewCreateInfo();
		ivi.image = buf.image;
		ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		ivi.components = {};
		ivi.format = swapd.ci.imageFormat;
		ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ivi.subresourceRange.layerCount = 1;
		ivi.subresourceRange.levelCount = 1;
		VK_CHECK(devd.dispatch.vkCreateImageView(device, &ivi, nullptr, &buf.view));

		VkFramebufferCreateInfo fbi = vk::FramebufferCreateInfo();
		fbi.attachmentCount = 1u;
		fbi.pAttachments = &buf.view;
		fbi.layers = 1u;
		fbi.width = swapd.ci.imageExtent.width;
		fbi.height = swapd.ci.imageExtent.height;
		fbi.renderPass = swapd.rp;
		VK_CHECK(devd.dispatch.vkCreateFramebuffer(device, &fbi, nullptr, &buf.fb));
	}

	swapd.imgui = ImGui::CreateContext();
	ImGui::SetCurrentContext(swapd.imgui);
	ImGui::GetIO().DisplaySize.x = swapd.ci.imageExtent.width;
	ImGui::GetIO().DisplaySize.y = swapd.ci.imageExtent.height;
	ImGui::GetIO().IniFilename = nullptr;

	{
		std::lock_guard lock(devd.mutex);
		auto [it, success] = devd.swapchains.emplace(*pSwapchain, std::move(pswapd));
		dlg_assert(success);
	}

	return result;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(
		VkDevice                                    device,
		VkSwapchainKHR                             	swapchain,
		const VkAllocationCallbacks*                pAllocator) {
	auto& devd = getData<Device>(device);

	{
		std::lock_guard lock(devd.mutex);
		auto it = devd.swapchains.find(swapchain);
		dlg_assert(it != devd.swapchains.end());
		devd.swapchains.erase(it);
	}

	devd.dispatch.vkDestroySwapchainKHR(device, swapchain, pAllocator);
}

Swapchain& getSwapchain(Device& dev, VkSwapchainKHR swapchain) {
	std::shared_lock lock(dev.mutex);
	auto it = dev.swapchains.find(swapchain);
	dlg_assert(it != dev.swapchains.end());
	return *it->second;
}

void drawGui(Swapchain& swapchain, Draw& draw) {
	(void) draw;
	auto& dev = *swapchain.dev;

	ImGui::NewFrame();
	if(ImGui::Begin("Images")) {
		for(auto& img : dev.images) {
			ImGui::Text("Name: %s", img.second->name.c_str());
		}
	}

	ImGui::End();
	ImGui::EndFrame();
	ImGui::Render();
}

void uploadDraw(Device& dev, Draw& draw) {
	auto drawData = *ImGui::GetDrawData();
	if(drawData.TotalIdxCount == 0) {
		return;
	}

	// make sure buffers are large enough
	auto vertexSize = drawData.TotalVtxCount * sizeof(ImDrawVert);
	auto vertexUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	draw.vertexBuffer.ensure(dev, vertexSize, vertexUsage);

	auto indexUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	auto indexSize = drawData.TotalIdxCount * sizeof(ImDrawIdx);
	draw.indexBuffer.ensure(dev, indexSize, indexUsage);

	// map
	ImDrawVert* verts;
	VK_CHECK(dev.dispatch.vkMapMemory(dev.dev, draw.vertexBuffer.mem, 0, vertexSize, 0, (void**) &verts));

	ImDrawIdx* inds;
	VK_CHECK(dev.dispatch.vkMapMemory(dev.dev, draw.indexBuffer.mem, 0, indexSize, 0, (void**) &inds));

	for(auto i = 0; i < drawData.CmdListsCount; ++i) {
		auto cmds = *drawData.CmdLists[i];
		std::memcpy(verts, cmds.VtxBuffer.Data, cmds.VtxBuffer.size() * sizeof(ImDrawVert));
		std::memcpy(inds, cmds.IdxBuffer.Data, cmds.IdxBuffer.size() * sizeof(ImDrawIdx));
		verts += cmds.VtxBuffer.Size;
		inds += cmds.IdxBuffer.Size;
	}

	VkMappedMemoryRange range[2] = {};
	range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range[0].memory = draw.vertexBuffer.mem;
	range[0].size = VK_WHOLE_SIZE;
	range[1].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range[1].memory = draw.indexBuffer.mem;
	range[1].size = VK_WHOLE_SIZE;

	VK_CHECK(dev.dispatch.vkFlushMappedMemoryRanges(dev.dev, 2, range));
	dev.dispatch.vkUnmapMemory(dev.dev, draw.vertexBuffer.mem);
	dev.dispatch.vkUnmapMemory(dev.dev, draw.indexBuffer.mem);
}

void recordDraw(Swapchain& swapchain, Draw& draw, u32 imageIdx) {
	auto& dev = *swapchain.dev;
	auto drawData = *ImGui::GetDrawData();
	if(drawData.TotalIdxCount == 0) {
		return;
	}

	VkRenderPassBeginInfo rpBegin = vk::RenderPassBeginInfo();
	rpBegin.renderArea.extent = swapchain.ci.imageExtent;
	rpBegin.renderPass = swapchain.rp;
	rpBegin.framebuffer = swapchain.buffers[imageIdx].fb;
	dev.dispatch.vkCmdBeginRenderPass(draw.cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport viewport {};
	viewport.width = swapchain.ci.imageExtent.width;
	viewport.height = swapchain.ci.imageExtent.height;
	viewport.maxDepth = 1.f;
	dev.dispatch.vkCmdSetViewport(draw.cb, 0, 1, &viewport);

	dev.dispatch.vkCmdBindPipeline(draw.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, swapchain.pipe);
	dev.dispatch.vkCmdBindDescriptorSets(draw.cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
		dev.pipeLayout, 0, 1, &draw.ds, 0, nullptr);

	VkDeviceSize off0 = 0u;
	dev.dispatch.vkCmdBindVertexBuffers(draw.cb, 0, 1, &draw.vertexBuffer.buf, &off0);
	dev.dispatch.vkCmdBindIndexBuffer(draw.cb, draw.indexBuffer.buf, 0, VK_INDEX_TYPE_UINT16);

	float pcr[4];
	// scale
    pcr[0] = 2.0f / drawData.DisplaySize.x;
    pcr[1] = 2.0f / drawData.DisplaySize.y;
    // translate
    pcr[2] = -1.0f - drawData.DisplayPos.x * pcr[0];
    pcr[3] = -1.0f - drawData.DisplayPos.y * pcr[1];
    dev.dispatch.vkCmdPushConstants(draw.cb, dev.pipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pcr), pcr);

	auto idxOff = 0u;
	auto vtxOff = 0u;
	for(auto i = 0; i < drawData.CmdListsCount; ++i) {
		auto& cmds = *drawData.CmdLists[i];
		for(auto j = 0; j < cmds.CmdBuffer.Size; ++j) {
			auto& cmd = cmds.CmdBuffer[j];

			// TODO
			VkRect2D scissor {};
			// scissor.offset.x = std::max<int>(cmd.ClipRect.x - drawData.DisplayPos.x, 0);
			// scissor.offset.y = std::max<int>(cmd.ClipRect.y - drawData.DisplayPos.y, 0);
			// scissor.extent.width = cmd.ClipRect.z - cmd.ClipRect.x;
			// scissor.extent.height = cmd.ClipRect.w - cmd.ClipRect.y;
			scissor.extent.width = viewport.width;
			scissor.extent.height = viewport.height;
			dev.dispatch.vkCmdSetScissor(draw.cb, 0, 1, &scissor);

			dev.dispatch.vkCmdDrawIndexed(draw.cb, cmd.ElemCount, 1, idxOff, vtxOff, 0);
			idxOff += cmd.ElemCount;
		}

		vtxOff += cmds.VtxBuffer.Size;
	}

	dev.dispatch.vkCmdEndRenderPass(draw.cb);
}

void ensureFontAtlas(Swapchain& swapchain, VkCommandBuffer cb) {
	if(swapchain.font.uploaded) {
		return;
	}

	auto& dev = *swapchain.dev;

	ImGuiIO& io = ImGui::GetIO();
	unsigned char* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	size_t uploadSize = width * height * 4 * sizeof(char);

	// Create atlas image
	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent.width = width;
	ici.extent.height = height;
	ici.extent.depth = 1;
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VK_CHECK(dev.dispatch.vkCreateImage(dev.dev, &ici, nullptr, &swapchain.font.image));

	VkMemoryRequirements fontImageReq;
	dev.dispatch.vkGetImageMemoryRequirements(dev.dev, swapchain.font.image, &fontImageReq);

	VkMemoryAllocateInfo iai = {};
	iai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	iai.allocationSize = fontImageReq.size;
	iai.memoryTypeIndex = findLSB(fontImageReq.memoryTypeBits & dev.deviceLocalMemTypeBits);
	VK_CHECK(dev.dispatch.vkAllocateMemory(dev.dev, &iai, nullptr, &swapchain.font.mem));
	VK_CHECK(dev.dispatch.vkBindImageMemory(dev.dev, swapchain.font.image, swapchain.font.mem, 0));

	// font image view
	VkImageViewCreateInfo ivi = {};
	ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ivi.image = swapchain.font.image;
	ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ivi.format = VK_FORMAT_R8G8B8A8_UNORM;
	ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	ivi.subresourceRange.levelCount = 1;
	ivi.subresourceRange.layerCount = 1;
	VK_CHECK(dev.dispatch.vkCreateImageView(dev.dev, &ivi, nullptr, &swapchain.font.view));

	// Create the upload buffer
	VkBufferCreateInfo bci = vk::BufferCreateInfo();
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = uploadSize;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VK_CHECK(dev.dispatch.vkCreateBuffer(dev.dev, &bci, nullptr, &swapchain.font.uploadBuf));

	VkMemoryRequirements uploadBufReq;
	dev.dispatch.vkGetBufferMemoryRequirements(dev.dev, swapchain.font.uploadBuf, &uploadBufReq);

	VkMemoryAllocateInfo uploadai = {};
	uploadai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	uploadai.allocationSize = uploadBufReq.size;
	uploadai.memoryTypeIndex = findLSB(uploadBufReq.memoryTypeBits & dev.hostVisibleMemTypeBits);
	VK_CHECK(dev.dispatch.vkAllocateMemory(dev.dev, &uploadai, nullptr, &swapchain.font.uploadMem));
	VK_CHECK(dev.dispatch.vkBindBufferMemory(dev.dev, swapchain.font.uploadBuf, swapchain.font.uploadMem, 0));

	// Upload to Buffer
	char* map = NULL;
	VK_CHECK(dev.dispatch.vkMapMemory(dev.dev, swapchain.font.uploadMem, 0, uploadSize, 0, (void**)(&map)));
	std::memcpy(map, pixels, uploadSize);

	VkMappedMemoryRange range[1] = {};
	range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range[0].memory = swapchain.font.uploadMem;
	range[0].size = uploadSize;
	VK_CHECK(dev.dispatch.vkFlushMappedMemoryRanges(dev.dev, 1, range));
	dev.dispatch.vkUnmapMemory(dev.dev, swapchain.font.uploadMem);

	// Copy buffer to image
	VkImageMemoryBarrier copyBarrier[1] = {};
	copyBarrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	copyBarrier[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	copyBarrier[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	copyBarrier[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	copyBarrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	copyBarrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	copyBarrier[0].image = swapchain.font.image;
	copyBarrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copyBarrier[0].subresourceRange.levelCount = 1;
	copyBarrier[0].subresourceRange.layerCount = 1;
	dev.dispatch.vkCmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_HOST_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, NULL, 0, NULL,
		1, copyBarrier);

	VkBufferImageCopy region = {};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.width = width;
	region.imageExtent.height = height;
	region.imageExtent.depth = 1;
	dev.dispatch.vkCmdCopyBufferToImage(cb,
		swapchain.font.uploadBuf,
		swapchain.font.image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &region);

	VkImageMemoryBarrier useBarrier[1] = {};
	useBarrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	useBarrier[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	useBarrier[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	useBarrier[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	useBarrier[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	useBarrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	useBarrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	useBarrier[0].image = swapchain.font.image;
	useBarrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	useBarrier[0].subresourceRange.levelCount = 1;
	useBarrier[0].subresourceRange.layerCount = 1;
	dev.dispatch.vkCmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0,
		0, NULL,
		0, NULL,
		1, useBarrier);

	// Store our identifier
	io.Fonts->TexID = (ImTextureID)(std::intptr_t)swapchain.font.view;
	swapchain.font.uploaded = true;
}

VkResult drawPresent(Queue& queue, Swapchain& swapchain,
		std::span<const VkSemaphore> semaphores, u32 imageIdx) {
	auto& dev = *queue.dev;

	// find free draw
	Draw* foundDraw = nullptr;
	for(auto& draw : swapchain.draws) {
		if(dev.dispatch.vkGetFenceStatus(dev.dev, draw.fence) == VK_SUCCESS) {
			VK_CHECK(dev.dispatch.vkResetFences(dev.dev, 1, &draw.fence));

			foundDraw = &draw;
			break;
		}
	}

	if(!foundDraw) {
		foundDraw = &swapchain.draws.emplace_back();
		*foundDraw = {};

		// init data
		VkCommandBufferAllocateInfo cbai = vk::CommandBufferAllocateInfo();
		cbai.commandBufferCount = 1u;
		cbai.commandPool = dev.commandPool;
		cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		VK_CHECK(dev.dispatch.vkAllocateCommandBuffers(dev.dev, &cbai, &foundDraw->cb));

		// command buffer is a dispatchable object
		dev.setDeviceLoaderData(dev.dev, foundDraw->cb);

		VkFenceCreateInfo fci = vk::FenceCreateInfo();
		VK_CHECK(dev.dispatch.vkCreateFence(dev.dev, &fci, nullptr, &foundDraw->fence));

		VkSemaphoreCreateInfo sci = vk::SemaphoreCreateInfo();
		VK_CHECK(dev.dispatch.vkCreateSemaphore(dev.dev, &sci, nullptr, &foundDraw->semaphore));

		VkDescriptorSetAllocateInfo dsai = vk::DescriptorSetAllocateInfo();
		dsai.descriptorPool = dev.dsPool;
		dsai.descriptorSetCount = 1u;
		dsai.pSetLayouts = &dev.dsLayout;
		VK_CHECK(dev.dispatch.vkAllocateDescriptorSets(dev.dev, &dsai, &foundDraw->ds));
	}

	auto& draw = *foundDraw;

	VkCommandBufferBeginInfo cbBegin = vk::CommandBufferBeginInfo();
	VK_CHECK(dev.dispatch.vkBeginCommandBuffer(draw.cb, &cbBegin));

	ImGui::SetCurrentContext(swapchain.imgui);

	ensureFontAtlas(swapchain, draw.cb);

	// TODO: not needed every frame, really
	// update descriptor
	VkDescriptorImageInfo dsii;
	dsii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	dsii.imageView = swapchain.font.view;

	VkWriteDescriptorSet write = vk::WriteDescriptorSet();
	write.descriptorCount = 1u;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.dstSet = draw.ds;
	write.pImageInfo = &dsii;

	dev.dispatch.vkUpdateDescriptorSets(dev.dev, 1, &write, 0, nullptr);

	// if there is a platform (for input stuff), update it
	if(swapchain.platform) {
		swapchain.platform->update();
	}

	// render gui
	drawGui(swapchain, draw);
	uploadDraw(dev, draw);
	recordDraw(swapchain, draw, imageIdx);

	dev.dispatch.vkEndCommandBuffer(draw.cb);

	// submit batch
	// TODO: handle case where application doesn't give us semaphore
	// (and different queues are used?)
	auto waitStages = std::make_unique<VkPipelineStageFlags[]>(semaphores.size());
	for(auto i = 0u; i < semaphores.size(); ++i) {
		waitStages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	}

	VkSubmitInfo submitInfo = vk::SubmitInfo();
	submitInfo.commandBufferCount = 1u;
	submitInfo.pCommandBuffers = &draw.cb;
	submitInfo.signalSemaphoreCount = 1u;
	submitInfo.pSignalSemaphores = &draw.semaphore;
	submitInfo.pWaitDstStageMask = waitStages.get();
	submitInfo.pWaitSemaphores = semaphores.data();
	submitInfo.waitSemaphoreCount = semaphores.size();

	auto res = dev.dispatch.vkQueueSubmit(dev.gfxQueue->queue, 1u, &submitInfo, draw.fence);
	if(res != VK_SUCCESS) {
		dlg_error("vkSubmit error: {}", vk::name(vk::Result(res)));
		return res;
	}

	// call down
	VkPresentInfoKHR presentInfo = vk::PresentInfoKHR();
	presentInfo.pImageIndices = &imageIdx;
	presentInfo.pWaitSemaphores = &draw.semaphore;
	presentInfo.waitSemaphoreCount = 1u;
	presentInfo.pSwapchains = &swapchain.swapchain;
	presentInfo.swapchainCount = 1u;

	return dev.dispatch.vkQueuePresentKHR(queue.queue, &presentInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(
		VkQueue                                     queue,
		const VkPresentInfoKHR*                     pPresentInfo) {
	auto& qd = getData<Queue>(queue);

	auto combinedResult = VK_SUCCESS;
	for(auto i = 0u; i < pPresentInfo->swapchainCount; ++i) {
		auto& swapchain = getSwapchain(*qd.dev, pPresentInfo->pSwapchains[i]);
		auto waitsems = std::span{pPresentInfo->pWaitSemaphores, pPresentInfo->waitSemaphoreCount};
		auto res = drawPresent(qd, swapchain, waitsems, pPresentInfo->pImageIndices[i]);

		if(pPresentInfo->pResults) {
			pPresentInfo->pResults[i] = res;
		}

		if(res != VK_SUCCESS && combinedResult == VK_SUCCESS) {
			combinedResult = res;
		}
	}

	return combinedResult;
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

static const std::unordered_map<std::string_view, void*> funcPtrTable {
#define FUEN_HOOK(fn) { "vk" # fn, (void *) fn }
#define FUEN_ALIAS(alias, fn) { "vk" # alias, (void *) ## fn }
   FUEN_HOOK(GetInstanceProcAddr),
   FUEN_HOOK(GetDeviceProcAddr),

   FUEN_HOOK(CreateInstance),
   FUEN_HOOK(DestroyInstance),

   FUEN_HOOK(CreateDevice),
   FUEN_HOOK(DestroyDevice),

   FUEN_HOOK(CreateImage),
   FUEN_HOOK(DestroyImage),

   FUEN_HOOK(CreateSwapchainKHR),
   FUEN_HOOK(DestroySwapchainKHR),

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

#undef FUEN_HOOK
#undef FUEN_ALIAS
};

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
