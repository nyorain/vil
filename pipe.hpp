#pragma once

#include "common.hpp"
#include "shader.hpp"

#include <memory>

namespace fuen {

enum class PipelineType {
	graphics,
	compute,
};

struct PipelineStage {
	VkPipelineStageFlagBits stage;
	std::shared_ptr<SpirvData> spirv;

	std::vector<VkSpecializationMapEntry> specEntries;
	std::vector<std::byte> specData;
};

struct Pipeline {
	Device* dev;
	VkPipeline handle;
	PipelineType type;
};

struct GraphicsPipeline : Pipeline {
	std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
	std::vector<VkViewport> viewports;
	std::vector<VkRect2D> scissors;
	std::vector<VkVertexInputAttributeDescription> vertexAttribs;
	std::vector<VkVertexInputBindingDescription> vertexBindings;

    VkPipelineVertexInputStateCreateInfo      vertexInputState;
    VkPipelineInputAssemblyStateCreateInfo    inputAssemblyState;
    VkPipelineTessellationStateCreateInfo     tessellationState;
    VkPipelineViewportStateCreateInfo         viewportState;
    VkPipelineRasterizationStateCreateInfo    rasterizationState;
    VkPipelineMultisampleStateCreateInfo      multisampleState;
    VkPipelineDepthStencilStateCreateInfo     depthStencilState;
    VkPipelineColorBlendStateCreateInfo       colorBlendState;
    VkPipelineDynamicStateCreateInfo          dynamicState;

	std::vector<PipelineStage> stages;
};

struct ComputePipeline : Pipeline {
	PipelineStage stage;
};

struct PipelineLayout {
	Device* dev;
	VkPipelineLayout handle;
	std::vector<DescriptorSetLayout*> descriptors;
	std::vector<vk::PushConstantRange> pushConstants;
};

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(
    VkDevice                                    device,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    createInfoCount,
    const VkGraphicsPipelineCreateInfo*         pCreateInfos,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipelines);

VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(
    VkDevice                                    device,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    createInfoCount,
    const VkComputePipelineCreateInfo*          pCreateInfos,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipelines);

VKAPI_ATTR void VKAPI_CALL DestroyPipeline(
    VkDevice                                    device,
    VkPipeline                                  pipeline,
    const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(
    VkDevice                                    device,
    const VkPipelineLayoutCreateInfo*           pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkPipelineLayout*                           pPipelineLayout);

VKAPI_ATTR void VKAPI_CALL DestroyPipelineLayout(
    VkDevice                                    device,
    VkPipelineLayout                            pipelineLayout,
    const VkAllocationCallbacks*                pAllocator);

} // namespace fuen
