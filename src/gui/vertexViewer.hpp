#pragma once

#include <fwd.hpp>
#include <gui/render.hpp>
#include <vk/vulkan.h>
#include <util/vec.hpp>
#include <vector>
#include <optional>

namespace fuen {

struct VertexViewer {
	Device* dev_ {};
	VkRenderPass rp_ {};

	nytl::Vec2f lastMousPos_ {};

	// OwnBuffer ubo_;
	// void* uboMap_ {};

	// VkDescriptorSetLayout dsLayout_ {};
	// VkDescriptorSet ds_ {};
	VkPipelineLayout pipeLayout_ {};

	// NOTE: could use way less pipes and instead just use a storage buffer
	// to assemble the vertices from in our vertex shader.
	// TODO: could at least cache shader modules.
	struct Pipe {
		VkFormat format;
		u32 stride;
		VkPrimitiveTopology topology;
		VkPipeline pipe;
	};

	std::vector<Pipe> pipes_ {};

	void init(Device& dev, VkRenderPass rp);
	VkPipeline createPipe(VkFormat format, u32 stride, VkPrimitiveTopology topo);

	// Assumes to be inside a render pass with
	// - a depth attachment
	// - a single color attachment
	// - viewport and scissor dynamic state bound
	// Uses the current imgui context.
	void imGuiDraw(VkCommandBuffer cb, const GraphicsPipeline& src,
			const CommandHookState& copies, std::optional<VkIndexType>,
			u32 offset, u32 drawCount, u32 vertexOffset);
};

} // namespace fuen

