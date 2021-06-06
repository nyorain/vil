#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <util/intrusive.hpp>
#include <vk/vulkan.h>

#include <memory>
#include <cstdlib>
#include <atomic>
#include <unordered_set>

namespace vil {

struct XfbPatchDesc;

struct PipelineLayout : DeviceHandle {
	VkPipelineLayout handle;
	std::vector<IntrusivePtr<DescriptorSetLayout>> descriptors;
	std::vector<VkPushConstantRange> pushConstants;

	// Intrusive ref count. Needed e.g. for command buffer recording
	std::atomic<u32> refCount {0};

	~PipelineLayout();
};

// See vulkan section "pipeline layout compatibility"
bool pushConstantCompatible(const PipelineLayout& a, const PipelineLayout& b);
bool compatibleForSetN(const PipelineLayout& a, const PipelineLayout& b,
		u32 N, bool considerPushConstants = true);

struct PipelineShaderStage {
	VkShaderStageFlagBits stage;
	IntrusivePtr<SpirvData> spirv;
	std::string entryPoint;

	std::vector<VkSpecializationMapEntry> specEntries;
	std::vector<std::byte> specData;

	PipelineShaderStage() = default;
	explicit PipelineShaderStage(Device& dev, const VkPipelineShaderStageCreateInfo&);
};

struct Pipeline : DeviceHandle {
	VkPipeline handle {};
	VkPipelineBindPoint type {};

	// TODO: strictly speaking don't need shared ownership of the handle here, just
	// of the data it holds. Separate handle and description into separate
	// shared-owned objects? On the other hand, shouldn't hurt to keep it
	// alive here either, should be cheap.
	IntrusivePtr<PipelineLayout> layout {};

protected:
	// Make sure Pipeline objects are not created.
	// Should always be GraphicsPipeline or ComputePipeline
	Pipeline() = default;
};

// Returns all shader stages a pipeline has.
span<const PipelineShaderStage> stages(const Pipeline& pipe);

struct GraphicsPipeline : Pipeline {
	std::shared_ptr<RenderPassDesc> renderPass {};
	u32 subpass {};

	std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
	std::vector<VkViewport> viewports;
	std::vector<VkRect2D> scissors;
	std::vector<VkVertexInputAttributeDescription> vertexAttribs;
	std::vector<VkVertexInputBindingDescription> vertexBindings;
	std::unordered_set<VkDynamicState> dynamicState;

	VkPipelineVertexInputStateCreateInfo      vertexInputState {};
	VkPipelineInputAssemblyStateCreateInfo    inputAssemblyState {};
	VkPipelineTessellationStateCreateInfo     tessellationState {};
	VkPipelineViewportStateCreateInfo         viewportState {};
	VkPipelineRasterizationStateCreateInfo    rasterizationState {};
	VkPipelineMultisampleStateCreateInfo      multisampleState {};
	VkPipelineDepthStencilStateCreateInfo     depthStencilState {};
	VkPipelineColorBlendStateCreateInfo       colorBlendState {};

	std::vector<PipelineShaderStage> stages;

	bool hasTessellation : 1;
	bool hasDepthStencil : 1;
	bool hasMeshShader : 1;

	IntrusivePtr<XfbPatchDesc> xfbPatch; // valid when we injected xfb

	~GraphicsPipeline();
};

struct ComputePipeline : Pipeline {
	PipelineShaderStage stage;
};

struct RayTracingPipeline : Pipeline {
	struct Group {
		VkRayTracingShaderGroupTypeKHR type;
		u32 general;
		u32 closestHit;
		u32 anyHit;
		u32 intersection;
	};

	// TODO: does not include pipeline libraries yet
	std::vector<PipelineShaderStage> stages;
	std::vector<Group> groups;
	std::unordered_set<VkDynamicState> dynamicState;
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

// No need to hook VkPipelineCache at all at the moment.

// VK_KHR_ray_tracing_pipeline
VKAPI_ATTR VkResult VKAPI_CALL CreateRayTracingPipelinesKHR(
    VkDevice                                    device,
    VkDeferredOperationKHR                      deferredOperation,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    createInfoCount,
    const VkRayTracingPipelineCreateInfoKHR*    pCreateInfos,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipelines);

VKAPI_ATTR VkResult VKAPI_CALL GetRayTracingCaptureReplayShaderGroupHandlesKHR(
    VkDevice                                    device,
    VkPipeline                                  pipeline,
    uint32_t                                    firstGroup,
    uint32_t                                    groupCount,
    size_t                                      dataSize,
    void*                                       pData);

VKAPI_ATTR VkDeviceSize VKAPI_CALL GetRayTracingShaderGroupStackSizeKHR(
    VkDevice                                    device,
    VkPipeline                                  pipeline,
    uint32_t                                    group,
    VkShaderGroupShaderKHR                      groupShader);

} // namespace vil
