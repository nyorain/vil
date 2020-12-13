#pragma once

#include "fwd.hpp"
#include "device.hpp"
#include <vector>

namespace fuen {

struct PushConstantData {
	std::vector<std::byte> data;
	std::vector<std::pair<VkDeviceSize, VkDeviceSize>> ranges; // (offset, size)[]
};

using PushConstantMap = std::unordered_map<VkShaderStageFlagBits, PushConstantData>;

struct BoundDescriptorSet {
	DescriptorSet* ds {};
	PipelineLayout* layout {};
	std::vector<u32> dynamicOffsets;
};

struct DescriptorState {
	std::vector<BoundDescriptorSet> descriptorSets;
	PushConstantMap pushConstants;

	void bind(PipelineLayout& layout, u32 firstSet,
		span<DescriptorSet* const> sets, span<const u32> offsets);
};

struct BoundVertexBuffer {
	Buffer* buffer {};
	VkDeviceSize offset {};
};

struct BoundIndexBuffer {
	Buffer* buffer {};
	VkIndexType type {};
	VkDeviceSize offset {};
};

struct DynamicStateDepthBias {
	float constant;
	float clamp;
	float slope;
};

struct GraphicsState : DescriptorState {
	BoundIndexBuffer indices;
	std::vector<BoundVertexBuffer> vertices;
	GraphicsPipeline* pipe;
	RenderPass* rp;

	struct StencilState {
		u32 writeMask;
		u32 compareMask;
		u32 reference;
	};

	struct {
		std::vector<VkViewport> viewports;
		std::vector<VkRect2D> scissors;
		float lineWidth;
		DynamicStateDepthBias depthBias;
		std::array<float, 4> blendConstants;
		float depthBoundsMin;
		float depthBoundsMax;

		StencilState stencilFront;
		StencilState stencilBack;
	} dynamic;
};

struct ComputeState : DescriptorState {
	ComputePipeline* pipe;
};

} // namespace fuen
