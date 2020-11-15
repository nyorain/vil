#pragma once

#include "common.hpp"
#include "shader.hpp"

#include <memory>
#include <unordered_set>

namespace fuen {

enum class PipelineType {
	graphics,
	compute,
};

struct PipelineShaderStage {
	VkShaderStageFlagBits stage;
	std::shared_ptr<SpirvData> spirv;
	std::string entryPoint;

	std::vector<VkSpecializationMapEntry> specEntries;
	std::vector<std::byte> specData;

	PipelineShaderStage() = default;
	explicit PipelineShaderStage(Device& dev, const VkPipelineShaderStageCreateInfo&);
};

struct Pipeline {
	Device* dev;
	VkPipeline handle;
	PipelineType type;
	PipelineLayout* layout;

protected:
	// Make sure Pipeline objects are not created.
	// Should always be GraphicsPipeline or ComputePipeline
	Pipeline() = default;

	Pipeline(Pipeline&&) = delete;
	Pipeline& operator=(Pipeline&&) = delete;
};

struct GraphicsPipeline : Pipeline {
	RenderPass* renderPass {};
	u32 subpass {};

	std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
	std::vector<VkViewport> viewports;
	std::vector<VkRect2D> scissors;
	std::vector<VkVertexInputAttributeDescription> vertexAttribs;
	std::vector<VkVertexInputBindingDescription> vertexBindings;
	std::unordered_set<VkDynamicState> dynamicState;

    VkPipelineVertexInputStateCreateInfo      vertexInputState;
    VkPipelineInputAssemblyStateCreateInfo    inputAssemblyState;
    VkPipelineTessellationStateCreateInfo     tessellationState;
    VkPipelineViewportStateCreateInfo         viewportState;
    VkPipelineRasterizationStateCreateInfo    rasterizationState;
    VkPipelineMultisampleStateCreateInfo      multisampleState;
    VkPipelineDepthStencilStateCreateInfo     depthStencilState;
    VkPipelineColorBlendStateCreateInfo       colorBlendState;

	std::vector<PipelineShaderStage> stages;

	bool hasTessellation : 1;
	bool hasDepthStencil : 1;
	bool hasMeshShader : 1;
};

struct ComputePipeline : Pipeline {
	PipelineShaderStage stage;
};

struct PipelineLayout {
	Device* dev;
	VkPipelineLayout handle;
	std::vector<DescriptorSetLayout*> descriptors;
	std::vector<VkPushConstantRange> pushConstants;
};

// See vulkan section "pipeline layout compatibility"
bool pushConstantCompatible(const PipelineLayout& a, const PipelineLayout& b);
bool compatibleForSetN(const PipelineLayout& a, const PipelineLayout& b, u32 N);

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
