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

struct GraphicsState : DescriptorState {
	BoundIndexBuffer indices;
	std::vector<BoundVertexBuffer> vertices;
	GraphicsPipeline* pipe;
	RenderPass* rp;

	// TODO: dynamic pipeline states
};

struct ComputeState : DescriptorState {
	ComputePipeline* pipe;
};

} // namespace fuen
