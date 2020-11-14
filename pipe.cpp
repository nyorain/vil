#include "pipe.hpp"
#include "data.hpp"

namespace fuen {

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(
		VkDevice                                    device,
		VkPipelineCache                             pipelineCache,
		uint32_t                                    createInfoCount,
		const VkGraphicsPipelineCreateInfo*         pCreateInfos,
		const VkAllocationCallbacks*                pAllocator,
		VkPipeline*                                 pPipelines) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.vkCreateGraphicsPipelines(device, pipelineCache,
		createInfoCount, pCreateInfos, pAllocator, pPipelines);
	return res;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(
		VkDevice                                    device,
		VkPipelineCache                             pipelineCache,
		uint32_t                                    createInfoCount,
		const VkComputePipelineCreateInfo*          pCreateInfos,
		const VkAllocationCallbacks*                pAllocator,
		VkPipeline*                                 pPipelines) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.vkCreateComputePipelines(device, pipelineCache,
		createInfoCount, pCreateInfos, pAllocator, pPipelines);
	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyPipeline(
		VkDevice                                    device,
		VkPipeline                                  pipeline,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.dispatch.vkDestroyPipeline(device, pipeline, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(
		VkDevice                                    device,
		const VkPipelineLayoutCreateInfo*           pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkPipelineLayout*                           pPipelineLayout) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.vkCreatePipelineLayout(device, pCreateInfo,
		pAllocator, pPipelineLayout);
	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyPipelineLayout(
		VkDevice                                    device,
		VkPipelineLayout                            pipelineLayout,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.dispatch.vkDestroyPipelineLayout(device, pipelineLayout, pAllocator);
}

} // namespace fuen
