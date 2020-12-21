#pragma once

#include <fwd.hpp>
#include <device.hpp>
#include <vector>

namespace fuen {

/*
struct PushConstantData {
	span<std::byte> data;
	span<std::pair<VkDeviceSize, VkDeviceSize>> ranges; // (offset, size)[]
};

using PushConstantMap = std::unordered_map<VkShaderStageFlagBits, PushConstantData>;
*/

struct BoundDescriptorSet {
	DescriptorSet* ds {};
	PipelineLayout* layout {};
	span<u32> dynamicOffsets;
};

struct DescriptorState {
	span<BoundDescriptorSet> descriptorSets;

	// TODO: fix with CommandAllocator
	// PushConstantMap pushConstants;

	void bind(CommandBuffer& cb, PipelineLayout& layout, u32 firstSet,
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
	span<BoundVertexBuffer> vertices;
	GraphicsPipeline* pipe;
	RenderPass* rp;

	struct StencilState {
		u32 writeMask;
		u32 compareMask;
		u32 reference;
	};

	struct {
		span<VkViewport> viewports;
		span<VkRect2D> scissors;
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

GraphicsState copy(CommandBuffer& cb, const GraphicsState& src);
ComputeState copy(CommandBuffer& cb, const ComputeState& src);

} // namespace fuen
